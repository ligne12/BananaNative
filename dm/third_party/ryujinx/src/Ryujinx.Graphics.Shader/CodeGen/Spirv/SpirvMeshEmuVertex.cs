using Ryujinx.Graphics.Shader.StructuredIr;
using Spv.Generator;
using System.Collections.Generic;
using System.Linq;
using static Spv.Specification;

namespace Ryujinx.Graphics.Shader.CodeGen.Spirv
{
    using SpvInstruction = Spv.Generator.Instruction;

    // nxnative: the vertex shader that draws an emulated mesh program's output (SpirvMeshEmuEntry.cs) without VK_EXT_mesh_shader: a non-indexed triangle list (point
    // list for a point program) of MaxPrimitives x 3 vertices a mesh workgroup. Vertex i of the draw is corner i % 3 of primitive slot i / 3, of workgroup slot / MaxPrimitives:
    // it reads the slot's packed indices (~0: an unused slot, every corner at (0, 0, 0, 0), a degenerate primitive the rasterizer drops), then its vertex's planes from
    // the workgroup's planes (MeshShaderInfo's layout) and its primitive's per-primitive values. Outputs as the mesh entry declares them: the position (and point size), one
    // vec4 per per-vertex generic location, one Flat vec4 per per-primitive location; a component no plane names is 0 (1 for w). The mesh emulation buffer is a storage
    // buffer at set 0 binding 1 (binding 0 is the translator's support buffer elsewhere; this shader has none), reflected as cb_slot 0xff offset 3
    public static class SpirvMeshEmuVertex
    {
        public const int Binding = 1;

        // The sizes of a program's share of a draw's region (the host lays the region out with them): words of planes a mesh workgroup, payload words a task
        // workgroup, max vertices, max primitives, point primitives
        public static (int PlaneWords, int PayloadWords, int MaxVertices, int MaxPrimitives, bool Points) Layout(bool isTask, int[] layout)
        {
            MeshShaderInfo mesh = new(isTask, layout);
            return (isTask ? 0 : mesh.PlaneWords, mesh.PayloadWords, mesh.MaxVertices, mesh.MaxPrimitives, mesh.Points);
        }

        public static byte[] Generate(int[] layout, out int[] vertexLocations, out int[] primitiveLocations)
        {
            MeshShaderInfo mesh = new(false, layout);
            Module m = new((1 << 16) | (3 << 8));
            m.AddCapability(Capability.Shader);
            m.SetMemoryModel(AddressingModel.Logical, MemoryModel.GLSL450);

            SpvInstruction tVoid = m.TypeVoid(), tU32 = m.TypeInt(32, false), tS32 = m.TypeInt(32, true), tF32 = m.TypeFloat(32), tBool = m.TypeBool();
            SpvInstruction tVec4 = m.TypeVector(tF32, 4);
            SpvInstruction U(uint v) => m.Constant(tU32, v);
            SpvInstruction F(float v) => m.Constant(tF32, v);
            List<SpvInstruction> io = [];

            // The buffer: struct { uint data[]; } (Uniform + BufferBlock: SPIR-V 1.3, as the translator's vertex shaders)
            SpvInstruction tRt = m.TypeRuntimeArray(tU32);
            m.Decorate(tRt, Decoration.ArrayStride, (LiteralInteger)4);
            SpvInstruction tSb = m.TypeStruct(true, tRt);
            m.Decorate(tSb, Decoration.BufferBlock);
            m.MemberDecorate(tSb, 0, Decoration.Offset, (LiteralInteger)0);
            m.MemberDecorate(tSb, 0, Decoration.NonWritable);
            SpvInstruction sb = m.Variable(m.TypePointer(StorageClass.Uniform, tSb), StorageClass.Uniform);
            m.Name(sb, "dm_mesh_emu");
            m.Decorate(sb, Decoration.DescriptorSet, (LiteralInteger)0);
            m.Decorate(sb, Decoration.Binding, (LiteralInteger)Binding);
            m.AddGlobalVariable(sb);
            SpvInstruction pU32 = m.TypePointer(StorageClass.Uniform, tU32);
            SpvInstruction LoadW(SpvInstruction index) => m.Load(tU32, m.AccessChain(pU32, sb, m.Constant(tS32, 0), index));

            SpvInstruction vidVar = m.Variable(m.TypePointer(StorageClass.Input, tS32), StorageClass.Input);
            m.Decorate(vidVar, Decoration.BuiltIn, (LiteralInteger)BuiltIn.VertexIndex);
            m.AddGlobalVariable(vidVar);
            io.Add(vidVar);

            bool pointSize = mesh.Points || mesh.Planes.Contains(MeshShaderInfo.PointSizePlane);
            SpvInstruction tPerVertex = pointSize ? m.TypeStruct(true, tVec4, tF32) : m.TypeStruct(true, tVec4);
            m.Decorate(tPerVertex, Decoration.Block);
            m.MemberDecorate(tPerVertex, 0, Decoration.BuiltIn, (LiteralInteger)BuiltIn.Position);
            if (pointSize)
            {
                m.MemberDecorate(tPerVertex, 1, Decoration.BuiltIn, (LiteralInteger)BuiltIn.PointSize);
            }
            SpvInstruction perVertex = m.Variable(m.TypePointer(StorageClass.Output, tPerVertex), StorageClass.Output);
            m.AddGlobalVariable(perVertex);
            io.Add(perVertex);

            vertexLocations = mesh.VertexLocations.ToArray();
            primitiveLocations = mesh.PrimitiveLocations.ToArray();
            Dictionary<int, SpvInstruction> outs = [], primOuts = [];
            foreach (int g in vertexLocations)
            {
                SpvInstruction v = m.Variable(m.TypePointer(StorageClass.Output, tVec4), StorageClass.Output);
                m.Decorate(v, Decoration.Location, (LiteralInteger)g);
                m.Name(v, $"mesh_out{g}");
                m.AddGlobalVariable(v);
                outs[g] = v;
                io.Add(v);
            }
            foreach (int g in primitiveLocations)
            {
                SpvInstruction v = m.Variable(m.TypePointer(StorageClass.Output, tVec4), StorageClass.Output);
                m.Decorate(v, Decoration.Location, (LiteralInteger)g);
                m.Decorate(v, Decoration.Flat);
                m.Name(v, $"mesh_prim{g}");
                m.AddGlobalVariable(v);
                primOuts[g] = v;
                io.Add(v);
            }

            SpvInstruction main = m.Function(tVoid, FunctionControlMask.MaskNone, m.TypeFunction(tVoid, false));
            m.Name(main, "main");
            m.AddFunction(main);
            m.AddLabel(m.Label());

            uint perPrimitive = mesh.Points ? 1u : 3u;
            SpvInstruction vid = m.Bitcast(tU32, m.Load(tS32, vidVar));
            SpvInstruction slot = m.UDiv(tU32, vid, U(perPrimitive));
            SpvInstruction corner = m.UMod(tU32, vid, U(perPrimitive));
            SpvInstruction group = m.UDiv(tU32, slot, U((uint)mesh.MaxPrimitives));
            SpvInstruction prim = m.UMod(tU32, slot, U((uint)mesh.MaxPrimitives));
            SpvInstruction packed = LoadW(m.IAdd(tU32, LoadW(U(SpirvMeshEmuEntry.HdrIndices)), slot));
            SpvInstruction live = m.INotEqual(tBool, packed, U(0xffffffff));
            SpvInstruction vertex = m.BitwiseAnd(tU32, m.ShiftRightLogical(tU32, packed, m.IMul(tU32, corner, U(8))), U(0xff));
            SpvInstruction planes = m.IAdd(tU32, LoadW(U(SpirvMeshEmuEntry.HdrPlanes)), m.IMul(tU32, group, U((uint)mesh.PlaneWords)));

            // Plane n of the vertex: (v / 32) x chunk + n x 32 + v % 32; value n of the primitive: the per-primitive base + (t / 32) x chunk + n x 32 + t % 32
            SpvInstruction vertexBase = m.IAdd(tU32, planes, m.IAdd(tU32, m.IMul(tU32, m.ShiftRightLogical(tU32, vertex, U(5)), U((uint)mesh.VertexChunkWords)), m.BitwiseAnd(tU32, vertex, U(31))));
            SpvInstruction primBase = m.IAdd(tU32, planes, m.IAdd(tU32, U((uint)mesh.PrimitiveBaseWord),
                m.IAdd(tU32, m.IMul(tU32, m.ShiftRightLogical(tU32, prim, U(5)), U((uint)mesh.PrimitiveChunkWords)), m.BitwiseAnd(tU32, prim, U(31)))));
            SpvInstruction zero = F(0f);
            SpvInstruction Live(SpvInstruction v) => m.Select(tF32, live, v, zero);
            SpvInstruction Plane(int n) => Live(m.Bitcast(tF32, LoadW(m.IAdd(tU32, vertexBase, U((uint)n * 32)))));
            SpvInstruction Value(int n) => Live(m.Bitcast(tF32, LoadW(m.IAdd(tU32, primBase, U((uint)n * 32)))));
            SpvInstruction Default(int code) => (code & 3) == 3 ? Live(F(1f)) : zero;

            SpvInstruction Component(int code, int[] codes, System.Func<int, SpvInstruction> read)
            {
                int n = System.Array.IndexOf(codes, code);
                return n >= 0 ? read(n) : Default(code);
            }

            SpvInstruction Vec(int baseCode, int[] codes, System.Func<int, SpvInstruction> read) =>
                m.CompositeConstruct(tVec4, Component(baseCode, codes, read), Component(baseCode + 1, codes, read), Component(baseCode + 2, codes, read), Component(baseCode + 3, codes, read));

            m.Store(m.AccessChain(m.TypePointer(StorageClass.Output, tVec4), perVertex, m.Constant(tS32, 0)), Vec(0, mesh.Planes, Plane));
            if (pointSize)
            {
                int n = System.Array.IndexOf(mesh.Planes, MeshShaderInfo.PointSizePlane);
                m.Store(m.AccessChain(m.TypePointer(StorageClass.Output, tF32), perVertex, m.Constant(tS32, 1)), n >= 0 ? Plane(n) : F(1f));
            }
            foreach ((int g, SpvInstruction v) in outs)
            {
                m.Store(v, Vec(4 + 4 * g, mesh.Planes, Plane));
            }
            foreach ((int g, SpvInstruction v) in primOuts)
            {
                m.Store(v, Vec(4 + 4 * g, mesh.PrimitivePlanes, Value));
            }

            m.Return();
            m.FunctionEnd();
            m.AddEntryPoint(ExecutionModel.Vertex, main, "main", io.ToArray());
            return m.Generate();
        }
    }
}
