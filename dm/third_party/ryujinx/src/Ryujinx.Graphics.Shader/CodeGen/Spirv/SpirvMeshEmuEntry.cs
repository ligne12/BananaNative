using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using System;
using System.Collections.Generic;
using static Spv.Specification;

namespace Ryujinx.Graphics.Shader.CodeGen.Spirv
{
    using SpvInstruction = Spv.Generator.Instruction;
    using SpvLiteralInteger = Spv.Generator.LiteralInteger;

    // nxnative: a Switch 2 task / mesh program for a device without VK_EXT_mesh_shader (the AYN Thor's Turnip and Qualcomm drivers: job "mesh_emu",
    // MeshShaderInfo.Emulated). The same translated body (function 0) runs in a GLCompute entry point, and its stage memory goes through the mesh emulation buffer
    // (MeshShaderInfo.EmuBinding: dmsh_abi.h DMSH_KIND_MESH_EMU), which the host then draws from with a vertex shader (vk_android_build.py mesh_emu_vs). The host
    // gives each draw its own region of the buffer; 32-bit words:
    //   [0..2] the mesh dispatch's indirect arguments: x, counted by the task workgroups (the host sets 0, 1, 1; without a task it sets the count)
    //   [4..7] the draw's indirect arguments: vertexCount, raised by the mesh workgroups to (workgroup + 1) x primitives x vertices a primitive; 1, 0, 0
    //   [8] the capacity C in workgroups (tasks and meshes), [9] the first workgroup (DrawMeshTasks' first), [10] the payloads' word offset, [11] the planes',
    //   [12] the indices', [13] 1 when a task launches the mesh workgroups, [16 ..) one (task workgroup, rank) pair a mesh workgroup
    // Task (one workgroup a dispatched task): CtaId = first + its workgroup; before the body the launch count (task memory word 1) is 0; after it the payload is
    //   copied to payloads + task x PayloadWords and the n mesh workgroups it launches take entries atomicAdd([0], n) ... onwards, (task, 0 .. n - 1)
    // Mesh (one workgroup a dispatch index g): with a task, (task, rank) = entry g, CtaId = rank and the payload copied in; without, CtaId = first + g. Its plane stores
    //   land at planes + g x PlaneWords + the store's word (MeshShaderInfo's own layout, which the vertex shader reads back); after the body each primitive t's
    //   indices go to indices + g x MaxPrimitives + t, i0 | i1 << 8 | i2 << 16 (a point's alone) below the primitive count and ~0 above it
    // A workgroup at or past C does nothing (the task count can exceed the region; those meshes are lost, not drawn from another draw's memory)
    static class SpirvMeshEmuEntry
    {
        public const uint HdrDispatchX = 0, HdrVertexCount = 4, HdrCapacity = 8, HdrFirst = 9, HdrPayloads = 10, HdrPlanes = 11, HdrIndices = 12, HdrHasTask = 13, HdrList = 16;
        private const uint MaxMeshWorkgroups = 65535;

        // Before the functions are generated: the body's CtaId reads (Instructions.GetStoragePointer) take this Private copy, which the entry point fills
        public static void DeclareCtaId(CodeGenContext context)
        {
            SpvInstruction v = context.Variable(context.TypePointer(StorageClass.Private, context.TypeVector(context.TypeU32(), 3)), StorageClass.Private);
            context.Name(v, "dm_mesh_emu_ctaid");
            context.AddGlobalVariable(v);
            context.MeshEmuCtaId = v;
        }

        public static void Generate(CodeGenContext context, MeshShaderInfo mesh)
        {
            SpvInstruction tVoid = context.TypeVoid(), tU32 = context.TypeU32(), tS32 = context.TypeS32(), tBool = context.TypeBool();
            SpvInstruction tUVec3 = context.TypeVector(tU32, 3);
            SpvInstruction U(uint v) => context.Constant(tU32, v);
            uint threads = (uint)Math.Max(1, context.Definitions.ComputeLocalSizeX * context.Definitions.ComputeLocalSizeY * context.Definitions.ComputeLocalSizeZ);
            List<SpvInstruction> io = new(context.GetMainInterface());

            SpvInstruction lidVar = context.Variable(context.TypePointer(StorageClass.Input, tU32), StorageClass.Input);
            context.Decorate(lidVar, Decoration.BuiltIn, (SpvLiteralInteger)BuiltIn.LocalInvocationIndex);
            context.AddGlobalVariable(lidVar);
            io.Add(lidVar);

            // The real workgroup ID: the body's own input variable when it declared one (a builtin may be in the interface once)
            IoDefinition ctaDef = new(StorageKind.Input, IoVariable.CtaId);
            SpvInstruction wgVar;
            if (!context.Inputs.TryGetValue(ctaDef, out wgVar))
            {
                wgVar = context.Variable(context.TypePointer(StorageClass.Input, tUVec3), StorageClass.Input);
                context.Decorate(wgVar, Decoration.BuiltIn, (SpvLiteralInteger)BuiltIn.WorkgroupId);
                context.AddGlobalVariable(wgVar);
                io.Add(wgVar);
            }

            SpvInstruction pSb = context.TypePointer(StorageClass.StorageBuffer, tU32);
            SpvInstruction sb = context.StorageBuffers[mesh.EmuBinding];
            SpvInstruction Word(SpvInstruction index) => context.AccessChain(pSb, sb, context.Constant(tS32, 0), index);
            SpvInstruction LoadW(SpvInstruction index) => context.Load(tU32, Word(index));
            void StoreW(SpvInstruction index, SpvInstruction value) => context.Store(Word(index), value);

            SpvInstruction pWg = context.TypePointer(StorageClass.Workgroup, tU32), pPriv = context.TypePointer(StorageClass.Private, tU32);
            SpvInstruction payload = mesh.PayloadMemoryId >= 0 ? context.SharedMemories[mesh.PayloadMemoryId] : null;

            // Per invocation: the workgroup's dispatch index (the plane store reads it) and a loop counter
            SpvInstruction groupVar = context.Variable(pPriv, StorageClass.Private);
            context.Name(groupVar, "dm_mesh_emu_group");
            context.AddGlobalVariable(groupVar);
            SpvInstruction loopVar = context.Variable(pPriv, StorageClass.Private);
            context.AddGlobalVariable(loopVar);

            (_, SpvInstruction body) = context.GetFunction(0);
            SpvInstruction wrapper = context.Function(tVoid, FunctionControlMask.MaskNone, context.TypeFunction(tVoid, false));
            context.Name(wrapper, "main");
            context.AddFunction(wrapper);
            context.AddLabel(context.Label());

            void Barrier() => context.ControlBarrier(U((uint)Scope.Workgroup), U((uint)Scope.Workgroup),
                U((uint)(MemorySemanticsMask.AcquireRelease | MemorySemanticsMask.WorkgroupMemory | MemorySemanticsMask.UniformMemory)));

            void Guarded(SpvInstruction cond, Action then)
            {
                SpvInstruction thenLabel = context.Label(), mergeLabel = context.Label();
                context.SelectionMerge(mergeLabel, SelectionControlMask.MaskNone);
                context.BranchConditional(cond, thenLabel, mergeLabel);
                context.AddLabel(thenLabel);
                then();
                context.Branch(mergeLabel);
                context.AddLabel(mergeLabel);
            }

            // Each invocation takes elements lid, lid + threads, ... below a constant count (unrolled)
            void ForEach(SpvInstruction lidValue, int count, Action<SpvInstruction> then)
            {
                for (uint k = 0; k * threads < count; k++)
                {
                    SpvInstruction t = k == 0 ? lidValue : context.IAdd(tU32, lidValue, U(k * threads));
                    if ((k + 1) * threads <= count)
                    {
                        then(t);
                    }
                    else
                    {
                        Guarded(context.ULessThan(tBool, t, U((uint)count)), () => then(t));
                    }
                }
            }

            // The same below a count known only when the shader runs: a loop
            void ForEachRuntime(SpvInstruction lidValue, SpvInstruction count, Action<SpvInstruction> then)
            {
                SpvInstruction header = context.Label(), loopBody = context.Label(), cont = context.Label(), merge = context.Label();
                context.Store(loopVar, lidValue);
                context.Branch(header);
                context.AddLabel(header);
                SpvInstruction k = context.Load(tU32, loopVar);
                SpvInstruction more = context.ULessThan(tBool, k, count);   // before the merge: OpLoopMerge must come right before the branch
                context.LoopMerge(merge, cont, LoopControlMask.MaskNone);
                context.BranchConditional(more, loopBody, merge);
                context.AddLabel(loopBody);
                then(k);
                context.Branch(cont);
                context.AddLabel(cont);
                context.Store(loopVar, context.IAdd(tU32, context.Load(tU32, loopVar), U(threads)));
                context.Branch(header);
                context.AddLabel(merge);
            }

            SpvInstruction lid = context.Load(tU32, lidVar);
            SpvInstruction first = context.IEqual(tBool, lid, U(0));
            SpvInstruction wg = context.CompositeExtract(tU32, context.Load(tUVec3, wgVar), (SpvLiteralInteger)0);
            SpvInstruction capacity = LoadW(U(HdrCapacity));
            SpvInstruction firstWg = LoadW(U(HdrFirst));
            context.Store(groupVar, wg);

            Guarded(context.ULessThan(tBool, wg, capacity), () =>
            {
                if (mesh.IsTask)
                {
                    context.Store(context.MeshEmuCtaId, context.CompositeConstruct(tUVec3, context.IAdd(tU32, wg, firstWg), U(0), U(0)));
                    Guarded(first, () => context.Store(context.AccessChain(pWg, payload, U(1)), U(0)));
                    Barrier();
                    context.FunctionCall(tVoid, body);
                    Barrier();

                    // The mesh workgroups this task launches: their entries from a base the first invocation takes
                    SpvInstruction baseVar = context.Variable(pWg, StorageClass.Workgroup);
                    context.Name(baseVar, "dm_mesh_emu_base");
                    context.AddGlobalVariable(baseVar);
                    SpvInstruction launch = context.Load(tU32, context.AccessChain(pWg, payload, U(1)));
                    launch = context.Select(tU32, context.ULessThan(tBool, launch, U(MaxMeshWorkgroups)), launch, U(MaxMeshWorkgroups));
                    Guarded(first, () => context.Store(baseVar, context.AtomicIAdd(tU32, Word(U(HdrDispatchX)), U((uint)Scope.Device), U(0), launch)));
                    Barrier();
                    SpvInstruction baseIndex = context.Load(tU32, baseVar);

                    SpvInstruction payloads = context.IAdd(tU32, LoadW(U(HdrPayloads)), context.IMul(tU32, wg, U((uint)mesh.PayloadWords)));
                    ForEach(lid, mesh.PayloadWords, w => StoreW(context.IAdd(tU32, payloads, w), context.Load(tU32, context.AccessChain(pWg, payload, w))));

                    ForEachRuntime(lid, launch, k =>
                    {
                        SpvInstruction e = context.IAdd(tU32, baseIndex, k);
                        Guarded(context.ULessThan(tBool, e, capacity), () =>
                        {
                            SpvInstruction at = context.IAdd(tU32, U(HdrList), context.IMul(tU32, e, U(2)));
                            StoreW(at, wg);
                            StoreW(context.IAdd(tU32, at, U(1)), k);
                        });
                    });
                }
                else
                {
                    SpvInstruction hasTask = context.INotEqual(tBool, LoadW(U(HdrHasTask)), U(0));
                    SpvInstruction entry = context.IAdd(tU32, U(HdrList), context.IMul(tU32, wg, U(2)));
                    SpvInstruction task = context.Select(tU32, hasTask, LoadW(entry), U(0));
                    SpvInstruction rank = context.Select(tU32, hasTask, LoadW(context.IAdd(tU32, entry, U(1))), context.IAdd(tU32, wg, firstWg));
                    context.Store(context.MeshEmuCtaId, context.CompositeConstruct(tUVec3, rank, U(0), U(0)));

                    if (payload != null)
                    {
                        SpvInstruction payloads = context.IAdd(tU32, LoadW(U(HdrPayloads)), context.IMul(tU32, task, U((uint)mesh.PayloadWords)));
                        ForEach(lid, mesh.PayloadWords, w => context.Store(context.AccessChain(pWg, payload, w),
                            context.Select(tU32, hasTask, LoadW(context.IAdd(tU32, payloads, w)), U(0))));
                    }

                    SpvInstruction prims = context.SharedMemories[mesh.PrimitiveMemoryId];
                    Guarded(first, () => context.Store(context.AccessChain(pWg, prims, U(3)), U(0)));
                    Barrier();
                    context.FunctionCall(tVoid, body);
                    Barrier();

                    SpvInstruction primCount = context.Load(tU32, context.AccessChain(pWg, prims, U(3)));
                    SpvInstruction lastVertex = U((uint)mesh.MaxVertices - 1);
                    SpvInstruction Index(SpvInstruction word)
                    {
                        SpvInstruction w = context.BitwiseAnd(tU32, word, U((uint)mesh.PrimitiveWords - 1));
                        SpvInstruction b = context.Load(tU32, context.AccessChain(pWg, prims, w));
                        return context.Select(tU32, context.ULessThan(tBool, b, U((uint)mesh.MaxVertices)), b, lastVertex);
                    }

                    SpvInstruction indices = context.IAdd(tU32, LoadW(U(HdrIndices)), context.IMul(tU32, wg, U((uint)mesh.MaxPrimitives)));
                    ForEach(lid, mesh.MaxPrimitives, t =>
                    {
                        SpvInstruction packed;
                        if (mesh.Points)
                        {
                            packed = Index(context.IAdd(tU32, t, U(4)));
                        }
                        else
                        {
                            SpvInstruction a = context.IAdd(tU32, context.IMul(tU32, t, U(3)), U(4));
                            packed = context.BitwiseOr(tU32, Index(a), context.BitwiseOr(tU32,
                                context.ShiftLeftLogical(tU32, Index(context.IAdd(tU32, a, U(1))), U(8)),
                                context.ShiftLeftLogical(tU32, Index(context.IAdd(tU32, a, U(2))), U(16))));
                        }
                        StoreW(context.IAdd(tU32, indices, t), context.Select(tU32, context.ULessThan(tBool, t, primCount), packed, U(0xffffffff)));
                    });

                    uint perPrimitive = mesh.Points ? 1u : 3u;
                    Guarded(first, () => context.AtomicUMax(tU32, Word(U(HdrVertexCount)), U((uint)Scope.Device), U(0),
                        context.IMul(tU32, context.IAdd(tU32, wg, U(1)), U((uint)mesh.MaxPrimitives * perPrimitive))));
                }
            });

            context.Return();
            context.FunctionEnd();

            // The plane stores (MeshShaderInfo's layout, kept as it is): word w of this workgroup's planes, for w below PlaneWords
            if (mesh.IsMesh && context.HasMeshPlaneStore)
            {
                SpvInstruction store = context.MeshPlaneStore;
                context.Name(store, "mesh_plane_store");
                context.AddFunction(store);
                SpvInstruction word = context.FunctionParameter(tU32), value = context.FunctionParameter(tU32);
                context.AddLabel(context.Label());
                Guarded(context.ULessThan(tBool, word, U((uint)mesh.PlaneWords)), () =>
                {
                    SpvInstruction g = context.Load(tU32, groupVar);
                    SpvInstruction at = context.IAdd(tU32, LoadW(U(HdrPlanes)), context.IAdd(tU32, context.IMul(tU32, g, U((uint)mesh.PlaneWords)), word));
                    StoreW(at, value);
                });
                context.Return();
                context.FunctionEnd();
            }

            foreach (SpvInstruction global in context.GlobalVariables)   // SPIR-V 1.4: every global, not only the inputs and outputs
            {
                if (!io.Contains(global))
                {
                    io.Add(global);
                }
            }

            context.AddEntryPoint(ExecutionModel.GLCompute, wrapper, "main", io.ToArray());
            context.AddExecutionMode(wrapper, ExecutionMode.LocalSize,
                (SpvLiteralInteger)context.Definitions.ComputeLocalSizeX, (SpvLiteralInteger)context.Definitions.ComputeLocalSizeY, (SpvLiteralInteger)context.Definitions.ComputeLocalSizeZ);
        }
    }
}
