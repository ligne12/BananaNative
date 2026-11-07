using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Optimizations
{
    static class GlobalToStorage
    {
        private const int DriverReservedCb = 0;

        enum LsMemoryType
        {
            Local,
            Shared,
        }

        private class GtsContext
        {
            private readonly struct Entry
            {
                public readonly int FunctionId;
                public readonly Instruction Inst;
                public readonly StorageKind StorageKind;
                public readonly bool IsMultiTarget;
                public readonly IReadOnlyList<uint> TargetCbs;

                public Entry(
                    int functionId,
                    Instruction inst,
                    StorageKind storageKind,
                    bool isMultiTarget,
                    IReadOnlyList<uint> targetCbs)
                {
                    FunctionId = functionId;
                    Inst = inst;
                    StorageKind = storageKind;
                    IsMultiTarget = isMultiTarget;
                    TargetCbs = targetCbs;
                }
            }

            private readonly struct LsKey : IEquatable<LsKey>
            {
                public readonly Operand BaseOffset;
                public readonly int ConstOffset;
                public readonly LsMemoryType Type;

                public LsKey(Operand baseOffset, int constOffset, LsMemoryType type)
                {
                    BaseOffset = baseOffset;
                    ConstOffset = constOffset;
                    Type = type;
                }

                public override int GetHashCode()
                {
                    return HashCode.Combine(BaseOffset, ConstOffset, Type);
                }

                public override bool Equals(object obj)
                {
                    return obj is LsKey other && Equals(other);
                }

                public bool Equals(LsKey other)
                {
                    return other.BaseOffset == BaseOffset && other.ConstOffset == ConstOffset && other.Type == Type;
                }
            }

            private readonly List<Entry> _entries;
            private readonly Dictionary<LsKey, Dictionary<uint, SearchResult>> _sharedEntries;
            private readonly HelperFunctionManager _hfm;

            public GtsContext(HelperFunctionManager hfm)
            {
                _entries = [];
                _sharedEntries = new Dictionary<LsKey, Dictionary<uint, SearchResult>>();
                _hfm = hfm;
            }

            // nxnative: the helpers of grouped raw loads (TryGroupRawLoads), by their words' byte offsets from the first
            public readonly Dictionary<string, int> RawGroups = new();

            public int AddRawGroup(string key, Function function)
            {
                int functionId = _hfm.AddFunction(function);

                RawGroups[key] = functionId;

                return functionId;
            }

            // nxnative: whether functionId is the helper of a raw global access (TryGenerateRawStorageOp: no target buffer)
            public bool IsRawFunction(int functionId)
            {
                return _entries.Exists(entry => entry.FunctionId == functionId && entry.IsMultiTarget && entry.TargetCbs.Count == 0);
            }

            public int AddFunction(Operation baseOp, bool isMultiTarget, IReadOnlyList<uint> targetCbs, Function function)
            {
                int functionId = _hfm.AddFunction(function);

                _entries.Add(new Entry(functionId, baseOp.Inst, baseOp.StorageKind, isMultiTarget, targetCbs));

                return functionId;
            }

            public bool TryGetFunctionId(Operation baseOp, bool isMultiTarget, List<uint> targetCbs, out int functionId)
            {
                foreach (Entry entry in _entries)
                {
                    if (entry.Inst != baseOp.Inst ||
                        entry.StorageKind != baseOp.StorageKind ||
                        entry.IsMultiTarget != isMultiTarget ||
                        entry.TargetCbs.Count != targetCbs.Count)
                    {
                        continue;
                    }

                    bool allEqual = true;

                    for (int index = 0; index < targetCbs.Count; index++)
                    {
                        if (targetCbs[index] != entry.TargetCbs[index])
                        {
                            allEqual = false;
                            break;
                        }
                    }

                    if (allEqual)
                    {
                        functionId = entry.FunctionId;
                        return true;
                    }
                }

                functionId = -1;
                return false;
            }

            public void AddMemoryTargetCb(LsMemoryType type, Operand baseOffset, int constOffset, uint targetCb, SearchResult result)
            {
                LsKey key = new(baseOffset, constOffset, type);

                if (!_sharedEntries.TryGetValue(key, out Dictionary<uint, SearchResult> targetCbs))
                {
                    // No entry with this base offset, create a new one.

                    targetCbs = new Dictionary<uint, SearchResult>() { { targetCb, result } };

                    _sharedEntries.Add(key, targetCbs);
                }
                else if (targetCbs.TryGetValue(targetCb, out SearchResult existingResult))
                {
                    // If our entry already exists, but does not match the new result,
                    // we set the offset to null to indicate there are multiple possible offsets.
                    // This will be used on the multi-target access that does not need to know the offset.

                    if (existingResult.Offset != null &&
                        (existingResult.Offset != result.Offset ||
                        existingResult.ConstOffset != result.ConstOffset))
                    {
                        targetCbs[targetCb] = new SearchResult(result.SbCbSlot, result.SbCbOffset);
                    }
                }
                else
                {
                    // An entry for this base offset already exists, but not for the specified
                    // constant buffer region where the storage buffer base address and size
                    // comes from.

                    targetCbs.Add(targetCb, result);
                }
            }

            public bool TryGetMemoryTargetCb(LsMemoryType type, Operand baseOffset, int constOffset, out SearchResult result)
            {
                LsKey key = new(baseOffset, constOffset, type);

                if (_sharedEntries.TryGetValue(key, out Dictionary<uint, SearchResult> targetCbs) && targetCbs.Count == 1)
                {
                    SearchResult candidateResult = targetCbs.Values.First();

                    if (candidateResult.Found)
                    {
                        result = candidateResult;

                        return true;
                    }
                }

                result = default;

                return false;
            }
        }

        private readonly struct SearchResult
        {
            public static SearchResult NotFound => new(-1, 0);
            public bool Found => SbCbSlot != -1;
            public int SbCbSlot { get; }
            public int SbCbOffset { get; }
            public Operand Offset { get; }
            public int ConstOffset { get; }

            public SearchResult(int sbCbSlot, int sbCbOffset)
            {
                SbCbSlot = sbCbSlot;
                SbCbOffset = sbCbOffset;
            }

            public SearchResult(int sbCbSlot, int sbCbOffset, Operand offset, int constOffset = 0)
            {
                SbCbSlot = sbCbSlot;
                SbCbOffset = sbCbOffset;
                Offset = offset;
                ConstOffset = constOffset;
            }
        }

        public static void RunPass(
            HelperFunctionManager hfm,
            BasicBlock[] blocks,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            TargetLanguage targetLanguage)
        {
            GtsContext gtsContext = new(hfm);

            foreach (BasicBlock block in blocks)
            {
                for (LinkedListNode<INode> node = block.Operations.First; node != null; node = node.Next)
                {
                    if (node.Value is not Operation operation)
                    {
                        continue;
                    }

                    if (IsGlobalMemory(operation.StorageKind))
                    {
                        LinkedListNode<INode> nextNode = ReplaceGlobalMemoryWithStorage(
                            gtsContext,
                            resourceManager,
                            gpuAccessor,
                            targetLanguage,
                            block,
                            node);

                        if (nextNode == null)
                        {
                            // The returned value being null means that the global memory replacement failed,
                            // so we just make loads read 0 and stores do nothing.

                            gpuAccessor.Log($"Failed to reserve storage buffer for global memory operation \"{operation.Inst}\".");

                            if (operation.Dest != null)
                            {
                                operation.TurnIntoCopy(Const(0));
                            }
                            else
                            {
                                Utils.DeleteNode(node, operation);
                            }
                        }
                        else
                        {
                            node = nextNode;
                        }
                    }
                    else if (operation.Inst == Instruction.Store &&
                        (operation.StorageKind == StorageKind.SharedMemory ||
                        operation.StorageKind == StorageKind.LocalMemory))
                    {
                        // The NVIDIA compiler can sometimes use shared or local memory as temporary
                        // storage to place the base address and size on, so we need
                        // to be able to find such information stored in memory too.

                        if (TryGetMemoryOffsets(operation, out LsMemoryType type, out Operand baseOffset, out int constOffset))
                        {
                            Operand value = operation.GetSource(operation.SourcesCount - 1);

                            SearchResult result = FindUniqueBaseAddressCb(gtsContext, block, value, needsOffset: false);
                            if (result.Found)
                            {
                                uint targetCb = PackCbSlotAndOffset(result.SbCbSlot, result.SbCbOffset);
                                gtsContext.AddMemoryTargetCb(type, baseOffset, constOffset, targetCb, result);
                            }
                        }
                    }
                }
            }
        }

        private static bool IsGlobalMemory(StorageKind storageKind)
        {
            return storageKind is StorageKind.GlobalMemory or
                   StorageKind.GlobalMemoryS8 or
                   StorageKind.GlobalMemoryS16 or
                   StorageKind.GlobalMemoryU8 or
                   StorageKind.GlobalMemoryU16;
        }

        private static bool IsSmallInt(StorageKind storageKind)
        {
            return storageKind is StorageKind.GlobalMemoryS8 or
                   StorageKind.GlobalMemoryS16 or
                   StorageKind.GlobalMemoryU8 or
                   StorageKind.GlobalMemoryU16;
        }

        private static LinkedListNode<INode> ReplaceGlobalMemoryWithStorage(
            GtsContext gtsContext,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            TargetLanguage targetLanguage,
            BasicBlock block,
            LinkedListNode<INode> node)
        {
            Operation operation = node.Value as Operation;
            Operand globalAddress = operation.GetSource(0);
            SearchResult result = FindUniqueBaseAddressCb(gtsContext, block, globalAddress, needsOffset: true);

            if (result.Found)
            {
                // We found the storage buffer that is being accessed.
                // There are two possible paths here, if the operation is simple enough,
                // we just generate the storage access code inline.
                // Otherwise, we generate a function call (and the function if necessary).

                Operand offset = result.Offset;

                bool storageUnaligned = gpuAccessor.QueryHasUnalignedStorageBuffer();

                if (storageUnaligned)
                {
                    Operand baseAddress = Cbuf(result.SbCbSlot, result.SbCbOffset);

                    Operand baseAddressMasked = Local();
                    Operand hostOffset = Local();

                    int alignment = gpuAccessor.QueryHostStorageBufferOffsetAlignment();

                    Operation maskOp = new(Instruction.BitwiseAnd, baseAddressMasked, baseAddress, Const(-alignment));
                    Operation subOp = new(Instruction.Subtract, hostOffset, globalAddress, baseAddressMasked);

                    node.List.AddBefore(node, maskOp);
                    node.List.AddBefore(node, subOp);

                    offset = hostOffset;
                }
                else if (result.ConstOffset != 0)
                {
                    Operand newOffset = Local();

                    Operation addOp = new(Instruction.Add, newOffset, offset, Const(result.ConstOffset));

                    node.List.AddBefore(node, addOp);

                    offset = newOffset;
                }

                if (CanUseInlineStorageOp(operation, targetLanguage))
                {
                    return GenerateInlineStorageOp(resourceManager, node, operation, offset, result);
                }
                else
                {
                    if (!TryGenerateSingleTargetStorageOp(
                        gtsContext,
                        resourceManager,
                        targetLanguage,
                        operation,
                        result,
                        out int functionId))
                    {
                        return null;
                    }

                    return GenerateCallStorageOp(node, operation, offset, functionId);
                }
            }
            else
            {
                // Failed to find the storage buffer directly.
                // Try to walk through Phi chains and find all possible constant buffers where
                // the base address might be stored.
                // Generate a helper function that will check all possible storage buffers and use the right one.

                if (!TryGenerateMultiTargetStorageOp(
                    gtsContext,
                    resourceManager,
                    gpuAccessor,
                    targetLanguage,
                    block,
                    operation,
                    out int functionId))
                {
                    return null;
                }

                if (TryGroupRawLoads(gtsContext, resourceManager, gpuAccessor, node, operation, functionId))
                {
                    return node;
                }

                return GenerateCallStorageOp(node, operation, null, functionId);
            }
        }

        private static bool CanUseInlineStorageOp(Operation operation, TargetLanguage targetLanguage)
        {
            if (operation.StorageKind != StorageKind.GlobalMemory)
            {
                return false;
            }

            return (operation.Inst != Instruction.AtomicMaxS32 &&
                    operation.Inst != Instruction.AtomicMinS32) || targetLanguage == TargetLanguage.Spirv;
        }

        private static LinkedListNode<INode> GenerateInlineStorageOp(
            ResourceManager resourceManager,
            LinkedListNode<INode> node,
            Operation operation,
            Operand offset,
            SearchResult result)
        {
            bool isStore = operation.Inst == Instruction.Store || operation.Inst.IsAtomic();
            if (!resourceManager.TryGetStorageBufferBinding(result.SbCbSlot, result.SbCbOffset, isStore, out int binding))
            {
                return null;
            }

            Operand wordOffset = Local();

            Operand[] sources;

            if (operation.Inst == Instruction.AtomicCompareAndSwap)
            {
                sources =
                [
                    Const(binding),
                    Const(0),
                    wordOffset,
                    operation.GetSource(operation.SourcesCount - 2),
                    operation.GetSource(operation.SourcesCount - 1)
                ];
            }
            else if (isStore)
            {
                sources = [Const(binding), Const(0), wordOffset, operation.GetSource(operation.SourcesCount - 1)];
            }
            else
            {
                sources = [Const(binding), Const(0), wordOffset];
            }

            Operation shiftOp = new(Instruction.ShiftRightU32, wordOffset, offset, Const(2));
            Operation storageOp = new(operation.Inst, StorageKind.StorageBuffer, operation.Dest, sources);

            node.List.AddBefore(node, shiftOp);
            LinkedListNode<INode> newNode = node.List.AddBefore(node, storageOp);

            Utils.DeleteNode(node, operation);

            return newNode;
        }

        private static LinkedListNode<INode> GenerateCallStorageOp(LinkedListNode<INode> node, Operation operation, Operand offset, int functionId)
        {
            // Generate call to a helper function that will perform the storage buffer operation.

            Operand[] sources = new Operand[operation.SourcesCount - 1 + (offset == null ? 2 : 1)];

            sources[0] = Const(functionId);

            if (offset != null)
            {
                // If the offset was supplised, we use that and skip the global address.

                sources[1] = offset;

                for (int srcIndex = 2; srcIndex < operation.SourcesCount; srcIndex++)
                {
                    sources[srcIndex] = operation.GetSource(srcIndex);
                }
            }
            else
            {
                // Use the 64-bit global address which is split in 2 32-bit arguments.

                for (int srcIndex = 0; srcIndex < operation.SourcesCount; srcIndex++)
                {
                    sources[srcIndex + 1] = operation.GetSource(srcIndex);
                }
            }

            bool returnsValue = operation.Dest != null;
            Operand returnValue = returnsValue ? Local() : null;

            Operation callOp = new(Instruction.Call, returnValue, sources);

            LinkedListNode<INode> newNode = node.List.AddBefore(node, callOp);

            if (returnsValue)
            {
                operation.TurnIntoCopy(returnValue);

                return node;
            }
            else
            {
                Utils.DeleteNode(node, operation);

                return newNode;
            }
        }

        private static bool TryGenerateSingleTargetStorageOp(
            GtsContext gtsContext,
            ResourceManager resourceManager,
            TargetLanguage targetLanguage,
            Operation operation,
            SearchResult result,
            out int functionId)
        {
            List<uint> targetCbs = [PackCbSlotAndOffset(result.SbCbSlot, result.SbCbOffset)];

            if (gtsContext.TryGetFunctionId(operation, isMultiTarget: false, targetCbs, out functionId))
            {
                return true;
            }

            int inArgumentsCount = 1;

            if (operation.Inst == Instruction.AtomicCompareAndSwap)
            {
                inArgumentsCount = 3;
            }
            else if (operation.Inst == Instruction.Store || operation.Inst.IsAtomic())
            {
                inArgumentsCount = 2;
            }

            EmitterContext context = new();

            Operand offset = Argument(0);
            Operand compare = null;
            Operand value = null;

            if (inArgumentsCount == 3)
            {
                compare = Argument(1);
                value = Argument(2);
            }
            else if (inArgumentsCount == 2)
            {
                value = Argument(1);
            }

            if (!TryGenerateStorageOp(
                resourceManager,
                targetLanguage,
                context,
                operation.Inst,
                operation.StorageKind,
                offset,
                compare,
                value,
                result,
                out Operand resultValue))
            {
                functionId = 0;
                return false;
            }

            bool returnsValue = resultValue != null;

            if (returnsValue)
            {
                context.Return(resultValue);
            }
            else
            {
                context.Return();
            }

            string functionName = GetFunctionName(operation, isMultiTarget: false, targetCbs);

            Function function = new(
                ControlFlowGraph.Create(context.GetOperations()).Blocks,
                functionName,
                returnsValue,
                inArgumentsCount,
                0);

            functionId = gtsContext.AddFunction(operation, isMultiTarget: false, targetCbs, function);

            return true;
        }

        private static bool TryGenerateMultiTargetStorageOp(
            GtsContext gtsContext,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            TargetLanguage targetLanguage,
            BasicBlock block,
            Operation operation,
            out int functionId)
        {
            Queue<PhiNode> phis = new();
            HashSet<PhiNode> visited = [];
            List<uint> targetCbs = [];

            Operand globalAddress = operation.GetSource(0);

            if (globalAddress.AsgOp is Operation addOp && addOp.Inst == Instruction.Add)
            {
                Operand src1 = addOp.GetSource(0);
                Operand src2 = addOp.GetSource(1);

                if (src1.Type == OperandType.Constant && src2.Type == OperandType.LocalVariable)
                {
                    globalAddress = src2;
                }
                else if (src1.Type == OperandType.LocalVariable && src2.Type == OperandType.Constant)
                {
                    globalAddress = src1;
                }
            }

            if (globalAddress.AsgOp is PhiNode phi && visited.Add(phi))
            {
                phis.Enqueue(phi);
            }
            else
            {
                SearchResult result = FindUniqueBaseAddressCb(gtsContext, block, operation.GetSource(0), needsOffset: false);

                if (result.Found)
                {
                    targetCbs.Add(PackCbSlotAndOffset(result.SbCbSlot, result.SbCbOffset));
                }
            }

            while (phis.TryDequeue(out phi))
            {
                for (int srcIndex = 0; srcIndex < phi.SourcesCount; srcIndex++)
                {
                    BasicBlock phiBlock = phi.GetBlock(srcIndex);
                    Operand phiSource = phi.GetSource(srcIndex);

                    SearchResult result = FindUniqueBaseAddressCb(gtsContext, phiBlock, phiSource, needsOffset: false);

                    if (result.Found)
                    {
                        uint targetCb = PackCbSlotAndOffset(result.SbCbSlot, result.SbCbOffset);

                        if (!targetCbs.Contains(targetCb))
                        {
                            targetCbs.Add(targetCb);
                        }
                    }
                    else if (phiSource.AsgOp is PhiNode phi2 && visited.Add(phi2))
                    {
                        phis.Enqueue(phi2);
                    }
                }
            }

            targetCbs.Sort();

            if (targetCbs.Count == 0)
            {
                gpuAccessor.Log($"Global memory operation \"{operation.Inst}\" traced to no storage buffer: it goes through the memory pool table.");
            }

            if (gtsContext.TryGetFunctionId(operation, isMultiTarget: true, targetCbs, out functionId))
            {
                return true;
            }

            if (targetCbs.Count == 0)
            {
                return TryGenerateRawStorageOp(gtsContext, resourceManager, gpuAccessor, operation, out functionId);
            }

            int inArgumentsCount = 2;

            if (operation.Inst == Instruction.AtomicCompareAndSwap)
            {
                inArgumentsCount = 4;
            }
            else if (operation.Inst == Instruction.Store || operation.Inst.IsAtomic())
            {
                inArgumentsCount = 3;
            }

            EmitterContext context = new();

            Operand globalAddressLow = Argument(0);
            Operand globalAddressHigh = Argument(1);

            foreach (uint targetCb in targetCbs)
            {
                (int sbCbSlot, int sbCbOffset) = UnpackCbSlotAndOffset(targetCb);

                Operand baseAddrLow = Cbuf(sbCbSlot, sbCbOffset);
                Operand baseAddrHigh = Cbuf(sbCbSlot, sbCbOffset + 1);
                Operand size = Cbuf(sbCbSlot, sbCbOffset + 2);

                Operand offset = context.ISubtract(globalAddressLow, baseAddrLow);
                Operand borrow = context.ICompareLessUnsigned(globalAddressLow, baseAddrLow);

                Operand inRangeLow = context.ICompareLessUnsigned(offset, size);

                Operand addrHighBorrowed = context.IAdd(globalAddressHigh, borrow);

                Operand inRangeHigh = context.ICompareEqual(addrHighBorrowed, baseAddrHigh);

                Operand inRange = context.BitwiseAnd(inRangeLow, inRangeHigh);

                Operand lblSkip = Label();
                context.BranchIfFalse(lblSkip, inRange);

                Operand compare = null;
                Operand value = null;

                if (inArgumentsCount == 4)
                {
                    compare = Argument(2);
                    value = Argument(3);
                }
                else if (inArgumentsCount == 3)
                {
                    value = Argument(2);
                }

                SearchResult result = new(sbCbSlot, sbCbOffset);

                int alignment = gpuAccessor.QueryHostStorageBufferOffsetAlignment();

                Operand baseAddressMasked = context.BitwiseAnd(baseAddrLow, Const(-alignment));
                Operand hostOffset = context.ISubtract(globalAddressLow, baseAddressMasked);

                if (!TryGenerateStorageOp(
                    resourceManager,
                    targetLanguage,
                    context,
                    operation.Inst,
                    operation.StorageKind,
                    hostOffset,
                    compare,
                    value,
                    result,
                    out Operand resultValue))
                {
                    functionId = 0;
                    return false;
                }

                if (resultValue != null)
                {
                    context.Return(resultValue);
                }
                else
                {
                    context.Return();
                }

                context.MarkLabel(lblSkip);
            }

            bool returnsValue = operation.Dest != null;

            if (returnsValue)
            {
                context.Return(Const(0));
            }
            else
            {
                context.Return();
            }

            string functionName = GetFunctionName(operation, isMultiTarget: true, targetCbs);

            Function function = new(
                ControlFlowGraph.Create(context.GetOperations()).Blocks,
                functionName,
                returnsValue,
                inArgumentsCount,
                0);

            functionId = gtsContext.AddFunction(operation, isMultiTarget: true, targetCbs, function);

            return true;
        }

        // nxnative: the memory pool table the host binds for raw global memory (dmsh_abi.h DMSH_GMEM_MAP_*): a storage buffer reserved under this sentinel constant buffer
        // slot, which no constant buffer of the game has (dmsh_build.py turns it into DMSH_KIND_GMEM_MAP). 1 << GmemMapLog2 entries of GmemMapEntryWords words each,
        // {start lo, start hi, end lo, end hi, delta lo, delta hi, 0, 0}, sorted by start; entry 0 is empty (start 0, end 0) and unused entries start at ~0
        public const int GmemMapCbSlot = 0xff;
        private const int GmemMapLog2 = 12;
        private const int GmemMapEntryWords = 8;
        private const int GmemMapSpanWord = 6;   // entry 0's word 6 (dmsh_abi.h DMSH_GMEM_MAP_SPAN_WORD): the search's first step is 1 << (GmemMapLog2 - 1) shifted right by it (0: the whole table)

        // nxnative: the Android host's page table in the same binding (IGpuAccessor.QueryGmemPageTable; dmsh_abi.h DMSH_GMEM_PT_*): words 0 to 1023 the null level 2 block (zeros),
        // from GmemPtL1Word one level 1 word for each 2 MiB block of the 39-bit address space (the word index of its level 2 array; 0 the null block), then the level 2 arrays:
        // two words for each 4 KiB page, the delta of the pool holding it with bit 0 set (deltas are multiples of 4), or a low word with bit 1 set outside every pool. Words 0 and 1
        // (the null block's first page) hold the host's zero page's device address with bit 1 set, which loads outside every pool read instead (EmitPageTableLoadAddress)
        private const int GmemPtL1Word = 1024;
        private const int GmemPtVaBits = 39;
        private const int GmemPtBlockLog2 = 21;
        private const int GmemPtPageLog2 = 12;

        // nxnative: the write log (dmsh_abi.h DMSH_GMEM_LOG_*), reserved under the same sentinel slot at offset 1: the set of guest pages (address >> 12) the stores and atomics wrote,
        // as an open-addressing hash set of 1 << GmemLogLog2 words from word 1 (0 = empty), word 0 set when a page found no free slot in GmemLogProbes tries. The host cannot know where
        // a store through a loaded pointer lands before the GPU runs it; it learns the pages from the log. A set, not a list: one draw of Donkey Kong Bananza stores 7,325 times into a few dozen pages
        private const int GmemLogCbOffset = 1;

        // nxnative: the shared memory spill buffer (StructuredIr/SharedSpillInfo.cs, dmsh_abi.h DMSH_SHARED_SPILL_CB_OFFSET), reserved under the same sentinel slot at offset 2
        public const int SharedSpillCbOffset = 2;

        // nxnative: an emulated task / mesh program's buffer (dmsh_abi.h DMSH_KIND_MESH_EMU, CodeGen/Spirv/SpirvMeshEmuEntry.cs)
        public const int MeshEmuCbOffset = 3;
        private const int GmemLogLog2 = 10;
        private const int GmemLogProbes = 8;

        // nxnative: a global memory access whose address the search traced to no storage buffer, an address the shader loaded from memory (Donkey Kong Bananza: gsys's vertex
        // shader that resets the terrain's indirect draw records reads each record's address from a storage buffer). Ryujinx drops such an access (a load reads 0). Here it
        // goes to the address itself: a branchless binary search finds the host's pool holding it, the address moves by that pool's delta (its Metal GPU address minus its
        // guest address) and is accessed through a physical storage buffer pointer. An address in no pool is still dropped
        private static bool TryGenerateRawStorageOp(
            GtsContext gtsContext,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            Operation operation,
            out int functionId)
        {
            functionId = 0;

            bool isSmall = IsSmallInt(operation.StorageKind);

            if (isSmall && operation.Inst.IsAtomic())
            {
                gpuAccessor.Log($"Global memory operation \"{operation.Inst}\" on a small integer traced to no storage buffer: not implemented.");
                return false;
            }

            if (!resourceManager.TryGetStorageBufferBinding(GmemMapCbSlot, 0, write: false, out int mapBinding))
            {
                return false;
            }

            bool writes = operation.Inst == Instruction.Store || operation.Inst.IsAtomic();
            int logBinding = 0;

            if (writes && !resourceManager.TryGetStorageBufferBinding(GmemMapCbSlot, GmemLogCbOffset, write: true, out logBinding))
            {
                return false;
            }

            resourceManager.Properties.UsesPhysicalPointers = true;

            int inArgumentsCount = 2;

            if (operation.Inst == Instruction.AtomicCompareAndSwap)
            {
                inArgumentsCount = 4;
            }
            else if (operation.Inst == Instruction.Store || operation.Inst.IsAtomic())
            {
                inArgumentsCount = 3;
            }

            EmitterContext context = new();

            Operand addrLow = Argument(0);
            Operand addrHigh = Argument(1);

            Operand MapWord(Operand entry, int word)
            {
                return context.Load(StorageKind.StorageBuffer, mapBinding, Const(0), context.IAdd(entry, Const(word)));
            }

            Operand deltaLow = Local();
            Operand deltaHigh = Local();
            Operand lblSkip = Label();
            Operand pageTableLow = null;
            Operand pageTableHigh = null;

            if (gpuAccessor.QueryGmemPageTable() && operation.Inst == Instruction.Load)
            {
                // A load takes its address with no branch (EmitPageTableLoadAddress): outside every pool it reads the zero page
                EmitPageTableLoadAddress(context, mapBinding, isSmall ? context.BitwiseAnd(addrLow, Const(~3)) : addrLow, addrHigh, out pageTableLow, out pageTableHigh);
            }
            else if (gpuAccessor.QueryGmemPageTable())
            {
                // The Android host's page table (dmsh_abi.h DMSH_GMEM_PT_*): a level 1 word for each 2 MiB of the 39-bit address space holds the word index of that block's
                // level 2 array (0: the null block), whose two words for each 4 KiB page are the pool's delta with bit 0 set, or 0 outside every pool. Three loads an access,
                // with no loop and no per-invocation state. The search below (12 steps and the pool cache) made one Ocarina of Time vertex shader with 253 accesses 50,769
                // instructions long once Turnip had inlined each, about half of them branches; its two draws took 21 ms of a frame's 40 ms of GPU on the AYN Thor
                EmitPageTableLookup(context, mapBinding, addrLow, addrHigh, deltaLow, deltaHigh, lblSkip);
            }
            else
            {
                // The invocation's last pool found (ResourceManager.GetGmemPoolCacheMemoryId: start, end and delta, zero at first: an empty range): an address in it takes its delta
                // without the search. A shader's accesses mostly fall in a few pools, and the search costs about 26 loads an access: one Ocarina of Time fragment shader makes 253 of
                // them a pixel, and its pass took 184 ms of GPU a frame on the AYN Thor
                int cacheId = resourceManager.GetGmemPoolCacheMemoryId();

                Operand CacheWord(int word)
                {
                    return context.Load(StorageKind.LocalMemory, cacheId, Const(word));
                }

                Operand lblFound = Label();

                Operand cacheStartLow = CacheWord(0);
                Operand cacheStartHigh = CacheWord(1);
                Operand cacheEndLow = CacheWord(2);
                Operand cacheEndHigh = CacheWord(3);

                Operand afterCacheStart = context.BitwiseOr(
                    context.ICompareLessUnsigned(cacheStartHigh, addrHigh),
                    context.BitwiseAnd(context.ICompareEqual(cacheStartHigh, addrHigh), context.ICompareLessOrEqualUnsigned(cacheStartLow, addrLow)));
                Operand beforeCacheEnd = context.BitwiseOr(
                    context.ICompareLessUnsigned(addrHigh, cacheEndHigh),
                    context.BitwiseAnd(context.ICompareEqual(addrHigh, cacheEndHigh), context.ICompareLessUnsigned(addrLow, cacheEndLow)));

                context.Copy(deltaLow, CacheWord(4));
                context.Copy(deltaHigh, CacheWord(5));
                context.BranchIfTrue(lblFound, context.BitwiseAnd(afterCacheStart, beforeCacheEnd));

                // The last entry whose start is at or below the address (entry 0 starts at 0, so there always is one), by a loop the driver cannot unroll: its first step is shifted
                // right by entry 0's word 6 (DMSH_GMEM_MAP_SPAN_WORD), which the hosts leave 0, so the trip count is a value only known when the shader runs. Unrolled (12 steps,
                // 2 loads each) and inlined by the driver into each of a shader's accesses, it made one Ocarina of Time fragment shader with 253 such accesses take 5 s to compile on Turnip
                Operand index = Local();
                Operand step = Local();
                context.Copy(index, Const(0));
                context.Copy(step, context.ShiftRightU32(Const(1 << (GmemMapLog2 - 1)), MapWord(Const(0), GmemMapSpanWord)));

                Operand lblSearch = Label();
                context.MarkLabel(lblSearch);

                Operand candidate = context.IAdd(index, step);
                Operand entry = context.IMultiply(candidate, Const(GmemMapEntryWords));
                Operand startLow = MapWord(entry, 0);
                Operand startHigh = MapWord(entry, 1);

                Operand atOrBelow = context.BitwiseOr(
                    context.ICompareLessUnsigned(startHigh, addrHigh),
                    context.BitwiseAnd(context.ICompareEqual(startHigh, addrHigh), context.ICompareLessOrEqualUnsigned(startLow, addrLow)));

                context.Copy(index, context.ConditionalSelect(atOrBelow, candidate, index));
                context.Copy(step, context.ShiftRightU32(step, Const(1)));
                context.BranchIfTrue(lblSearch, context.ICompareNotEqual(step, Const(0)));

                Operand found = context.IMultiply(index, Const(GmemMapEntryWords));
                Operand endLow = MapWord(found, 2);
                Operand endHigh = MapWord(found, 3);

                Operand inPool = context.BitwiseOr(
                    context.ICompareLessUnsigned(addrHigh, endHigh),
                    context.BitwiseAnd(context.ICompareEqual(addrHigh, endHigh), context.ICompareLessUnsigned(addrLow, endLow)));

                context.BranchIfFalse(lblSkip, inPool);

                context.Copy(deltaLow, MapWord(found, 4));
                context.Copy(deltaHigh, MapWord(found, 5));
                context.Store(StorageKind.LocalMemory, cacheId, Const(0), MapWord(found, 0));
                context.Store(StorageKind.LocalMemory, cacheId, Const(1), MapWord(found, 1));
                context.Store(StorageKind.LocalMemory, cacheId, Const(2), endLow);
                context.Store(StorageKind.LocalMemory, cacheId, Const(3), endHigh);
                context.Store(StorageKind.LocalMemory, cacheId, Const(4), deltaLow);
                context.Store(StorageKind.LocalMemory, cacheId, Const(5), deltaHigh);

                context.MarkLabel(lblFound);
            }

            if (writes)
            {
                // A page in a pool is never 0 (the guest's first page is not mapped), so 0 marks an empty slot
                Operand page = context.BitwiseOr(context.ShiftLeft(addrHigh, Const(20)), context.ShiftRightU32(addrLow, Const(12)));
                Operand hash = context.ShiftRightU32(context.IMultiply(page, Const(unchecked((int)0x9e3779b1))), Const(32 - GmemLogLog2));

                Operand lblLogged = Label();

                for (int probe = 0; probe < GmemLogProbes; probe++)
                {
                    Operand slot = context.IAdd(context.BitwiseAnd(context.IAdd(hash, Const(probe)), Const((1 << GmemLogLog2) - 1)), Const(1));
                    Operand old = context.AtomicCompareAndSwap(StorageKind.StorageBuffer, logBinding, Const(0), slot, Const(0), page);
                    context.BranchIfTrue(lblLogged, context.BitwiseOr(context.ICompareEqual(old, Const(0)), context.ICompareEqual(old, page)));
                }

                context.Store(StorageKind.StorageBuffer, logBinding, Const(0), Const(0), Const(1));
                context.MarkLabel(lblLogged);
            }

            // A small integer is read or written in its 32-bit word
            Operand hostLow = pageTableLow;
            Operand hostHigh = pageTableHigh;

            if (hostLow == null)
            {
                Operand wordLow = isSmall ? context.BitwiseAnd(addrLow, Const(~3)) : addrLow;
                hostLow = context.IAdd(wordLow, deltaLow);
                Operand carry = context.ConditionalSelect(context.ICompareLessUnsigned(hostLow, wordLow), Const(1), Const(0));
                hostHigh = context.IAdd(context.IAdd(addrHigh, deltaHigh), carry);
            }

            Operand resultValue = null;

            if (operation.Inst == Instruction.Load)
            {
                resultValue = context.Load(StorageKind.GlobalMemory, hostLow, hostHigh);

                if (isSmall)
                {
                    Operand bitOffset = HelperFunctionManager.GetBitOffset(context, addrLow);

                    resultValue = operation.StorageKind switch
                    {
                        StorageKind.GlobalMemoryS8 => context.BitfieldExtractS32(context.ShiftRightS32(resultValue, bitOffset), Const(0), Const(8)),
                        StorageKind.GlobalMemoryS16 => context.BitfieldExtractS32(context.ShiftRightS32(resultValue, bitOffset), Const(0), Const(16)),
                        StorageKind.GlobalMemoryU8 => context.BitwiseAnd(context.ShiftRightU32(resultValue, bitOffset), Const(byte.MaxValue)),
                        _ => context.BitwiseAnd(context.ShiftRightU32(resultValue, bitOffset), Const(ushort.MaxValue)),
                    };
                }
            }
            else if (operation.Inst == Instruction.Store)
            {
                Operand value = Argument(2);

                if (isSmall)
                {
                    int bitSize = operation.StorageKind is StorageKind.GlobalMemoryS8 or StorageKind.GlobalMemoryU8 ? 8 : 16;
                    Operand bitOffset = HelperFunctionManager.GetBitOffset(context, addrLow);

                    Operand lblLoopHead = Label();
                    context.MarkLabel(lblLoopHead);

                    Operand oldValue = context.Load(StorageKind.GlobalMemory, hostLow, hostHigh);
                    Operand newValue = context.BitfieldInsert(oldValue, value, bitOffset, Const(bitSize));
                    Operand casResult = context.AtomicCompareAndSwap(StorageKind.GlobalMemory, hostLow, hostHigh, oldValue, newValue);

                    context.BranchIfTrue(lblLoopHead, context.ICompareNotEqual(casResult, oldValue));
                }
                else
                {
                    context.Store(StorageKind.GlobalMemory, hostLow, hostHigh, value);
                }
            }
            else if (operation.Inst == Instruction.AtomicCompareAndSwap)
            {
                resultValue = context.AtomicCompareAndSwap(StorageKind.GlobalMemory, hostLow, hostHigh, Argument(2), Argument(3));
            }
            else
            {
                resultValue = context.Add(operation.Inst, StorageKind.GlobalMemory, Local(), hostLow, hostHigh, Argument(2));
            }

            if (resultValue != null)
            {
                context.Return(resultValue);
            }
            else
            {
                context.Return();
            }

            bool returnsValue = operation.Dest != null;

            if (pageTableLow == null)
            {
                context.MarkLabel(lblSkip);

                if (returnsValue)
                {
                    context.Return(Const(0));
                }
                else
                {
                    context.Return();
                }
            }

            Function function = new(
                ControlFlowGraph.Create(context.GetOperations()).Blocks,
                GetFunctionName(operation, isMultiTarget: true, []) + "Raw",
                returnsValue,
                inArgumentsCount,
                0);

            functionId = gtsContext.AddFunction(operation, isMultiTarget: true, [], function);

            return true;
        }

        // nxnative: the Android host's page table lookup of the address (addrLow, addrHigh): its pool's delta into deltaLow / deltaHigh, or a branch to lblSkip outside
        // every pool. Three loads, with no loop and no per-invocation state (dmsh_abi.h DMSH_GMEM_PT_*)
        private static void EmitPageTableLookup(EmitterContext context, int mapBinding, Operand addrLow, Operand addrHigh, Operand deltaLow, Operand deltaHigh, Operand lblSkip)
        {
            Operand MapWord(Operand entry, int word)
            {
                return context.Load(StorageKind.StorageBuffer, mapBinding, Const(0), context.IAdd(entry, Const(word)));
            }

            Operand inSpace = context.ICompareLessUnsigned(addrHigh, Const(1 << (GmemPtVaBits - 32)));
            Operand l1 = context.ConditionalSelect(inSpace, context.BitwiseOr(context.ShiftLeft(addrHigh, Const(32 - GmemPtBlockLog2)), context.ShiftRightU32(addrLow, Const(GmemPtBlockLog2))), Const(0));
            Operand block = MapWord(l1, GmemPtL1Word);
            Operand page = context.BitwiseAnd(context.ShiftRightU32(addrLow, Const(GmemPtPageLog2)), Const((1 << (GmemPtBlockLog2 - GmemPtPageLog2)) - 1));
            Operand pageEntry = context.IAdd(block, context.ShiftLeft(page, Const(1)));
            Operand ptLow = MapWord(pageEntry, 0);
            context.Copy(deltaHigh, MapWord(pageEntry, 1));
            context.BranchIfFalse(lblSkip, context.ICompareNotEqual(context.BitwiseAnd(ptLow, Const(1)), Const(0)));
            context.Copy(deltaLow, context.BitwiseAnd(ptLow, Const(~1)));
        }

        // nxnative: the host address a load of (addrLow, addrHigh) reads, with no branch: the address plus its pool's delta, or, outside every pool (the page's low word has
        // bit 1 set), the host's zero page (words 0 and 1) plus the address's offset in its page, which reads 0 as the skipped access did. A branch after each lookup split a
        // shader into a block per access, and Turnip ran each access's three dependent table loads and its own load one after the other: Ocarina of Time's vertex shader
        // b1f06157308e3b22 (71 lookups, 7,321 instructions) took 10.7 ms of a 49.5 ms frame of GPU on the AYN Thor (DM_VK_DRAW_SKIP)
        private static void EmitPageTableLoadAddress(EmitterContext context, int mapBinding, Operand addrLow, Operand addrHigh, out Operand hostLow, out Operand hostHigh)
        {
            Operand MapWord(Operand entry, int word)
            {
                return context.Load(StorageKind.StorageBuffer, mapBinding, Const(0), context.IAdd(entry, Const(word)));
            }

            Operand inSpace = context.ICompareLessUnsigned(addrHigh, Const(1 << (GmemPtVaBits - 32)));
            Operand l1 = context.ConditionalSelect(inSpace, context.BitwiseOr(context.ShiftLeft(addrHigh, Const(32 - GmemPtBlockLog2)), context.ShiftRightU32(addrLow, Const(GmemPtBlockLog2))), Const(0));
            Operand block = MapWord(l1, GmemPtL1Word);
            Operand page = context.BitwiseAnd(context.ShiftRightU32(addrLow, Const(GmemPtPageLog2)), Const((1 << (GmemPtBlockLog2 - GmemPtPageLog2)) - 1));
            Operand pageEntry = context.IAdd(block, context.ShiftLeft(page, Const(1)));
            Operand ptLow = MapWord(pageEntry, 0);
            Operand ptHigh = MapWord(pageEntry, 1);
            Operand outside = context.ICompareNotEqual(context.BitwiseAnd(ptLow, Const(2)), Const(0));
            Operand baseLow = context.BitwiseAnd(context.ConditionalSelect(outside, MapWord(Const(0), 0), ptLow), Const(~3));
            Operand baseHigh = context.ConditionalSelect(outside, MapWord(Const(0), 1), ptHigh);
            Operand offsetLow = context.ConditionalSelect(outside, context.BitwiseAnd(addrLow, Const((1 << GmemPtPageLog2) - 1)), addrLow);
            Operand offsetHigh = context.ConditionalSelect(outside, Const(0), addrHigh);

            hostLow = context.IAdd(baseLow, offsetLow);
            hostHigh = context.IAdd(context.IAdd(baseHigh, offsetHigh), context.ConditionalSelect(context.ICompareLessUnsigned(hostLow, baseLow), Const(1), Const(0)));
        }

        // nxnative: the words of one vector load share one page table lookup. The Ampere frontend emits LDG.64 / LDG.128 as a 32-bit load a word, at the same base plus
        // 4, 8 and 12 (InstEmitAmpereMem.ALdg); through the raw path each word made its own lookup, two dependent loads before its own. The loads of a block at one base
        // (an address or that address plus a constant, with its carry) within 16 bytes of each other, with no memory write between, become one call: the helper looks
        // the first word's page up and reads every word with that pool's delta. Words that close are in one pool, which is contiguous on both sides. Ocarina of Time's
        // vertex shader 4671e5e647fc5455 reads 58 LDG.128 of light probe data a vertex: 232 lookups before, 58 after. The helper has no branch (EmitPageTableLoadAddress)
        private static bool TryGroupRawLoads(
            GtsContext gtsContext,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            LinkedListNode<INode> node,
            Operation operation,
            int functionId)
        {
            if (!gpuAccessor.QueryGmemPageTable() ||
                operation.Inst != Instruction.Load ||
                operation.StorageKind != StorageKind.GlobalMemory ||
                operation.Dest == null ||
                operation.SourcesCount != 2 ||
                !gtsContext.IsRawFunction(functionId))
            {
                return false;
            }

            List<(Operation Op, LinkedListNode<INode> Node, int Delta)> best = null;

            foreach ((Operand baseLow, Operand baseHigh, int baseOffset) in BaseCandidates(operation.GetSource(0), operation.GetSource(1)))
            {
                List<(Operation Op, LinkedListNode<INode> Node, int Delta)> members = [(operation, node, 0)];
                int min = 0;
                int max = 0;
                int scanned = 0;

                for (LinkedListNode<INode> next = node.Next; next != null && scanned < 48; next = next.Next, scanned++)
                {
                    if (next.Value is not Operation other)
                    {
                        continue;
                    }

                    if (other.Inst == Instruction.Load && other.StorageKind == StorageKind.GlobalMemory && other.Dest != null && other.SourcesCount == 2)
                    {
                        foreach ((Operand low, Operand high, int offset) in BaseCandidates(other.GetSource(0), other.GetSource(1)))
                        {
                            if (low == baseLow && high == baseHigh)
                            {
                                int delta = offset - baseOffset;

                                if (Math.Max(max, delta) - Math.Min(min, delta) < 16 && !members.Exists(member => member.Delta == delta))
                                {
                                    members.Add((other, next, delta));
                                    min = Math.Min(min, delta);
                                    max = Math.Max(max, delta);
                                }

                                break;
                            }
                        }

                        continue;
                    }

                    Instruction inst = other.Inst & Instruction.Mask;

                    if (inst == Instruction.Store || inst == Instruction.Call || inst == Instruction.Barrier || inst == Instruction.MemoryBarrier ||
                        inst == Instruction.GroupMemoryBarrier || other.Inst.IsAtomic())
                    {
                        break;
                    }
                }

                if (members.Count > 1 && (best == null || members.Count > best.Count))
                {
                    best = members;
                }
            }

            if (best == null || !resourceManager.TryGetStorageBufferBinding(GmemMapCbSlot, 0, write: false, out int mapBinding))
            {
                return false;
            }

            string key = string.Join(",", best.ConvertAll(member => member.Delta));

            if (!gtsContext.RawGroups.TryGetValue(key, out int groupId))
            {
                EmitterContext context = new();

                Operand addrLow = Argument(0);
                Operand addrHigh = Argument(1);

                // (no branch: outside every pool the words come from the zero page, 8 KiB, so a page's last word plus 12 bytes is still in it)
                EmitPageTableLoadAddress(context, mapBinding, addrLow, addrHigh, out Operand hostLow, out Operand hostHigh);

                Operand first = null;

                for (int index = 0; index < best.Count; index++)
                {
                    int delta = best[index].Delta;
                    Operand wordLow = hostLow;
                    Operand wordHigh = hostHigh;

                    if (delta != 0)
                    {
                        wordLow = context.IAdd(hostLow, Const(delta));
                        wordHigh = context.IAdd(hostHigh, delta > 0
                            ? context.ConditionalSelect(context.ICompareLessUnsigned(wordLow, hostLow), Const(1), Const(0))
                            : context.ConditionalSelect(context.ICompareGreaterUnsigned(wordLow, hostLow), Const(-1), Const(0)));
                    }

                    Operand value = context.Load(StorageKind.GlobalMemory, wordLow, wordHigh);

                    if (index == 0)
                    {
                        first = value;
                    }
                    else
                    {
                        context.Copy(Argument(1 + index), value);
                    }
                }

                context.Return(first);

                Function function = new(
                    ControlFlowGraph.Create(context.GetOperations()).Blocks,
                    "LoadGlobalRawGroup_" + key.Replace(",", "_").Replace("-", "m"),
                    true,
                    2,
                    best.Count - 1);

                groupId = gtsContext.AddRawGroup(key, function);
            }

            Operand[] dests = new Operand[best.Count];

            for (int index = 0; index < dests.Length; index++)
            {
                dests[index] = Local();
            }

            Operation callOp = new(Instruction.Call, 0, dests, [Const(groupId), operation.GetSource(0), operation.GetSource(1)]);

            node.List.AddBefore(node, callOp);

            // The first load becomes a copy where it is (the pass goes on after it); the others are replaced by plain copies, since an operation turned into a copy keeps
            // its storage kind and the pass would take it for a global access when it reaches it
            operation.TurnIntoCopy(dests[0]);

            for (int index = 1; index < best.Count; index++)
            {
                (Operation other, LinkedListNode<INode> otherNode, _) = best[index];
                Operand dest = other.Dest;

                other.Dest = null;
                otherNode.List.AddBefore(otherNode, new Operation(Instruction.Copy, dest, dests[index]));
                Utils.DeleteNode(otherNode, other);
            }

            return true;
        }

        // The bases an address (low, high) can be read at: itself at offset 0, and, when its low word is a sum with a constant whose carry goes into the high word
        // (InstEmitAmpereMem.AGlobalAddress), that sum's other term and the high word before the carry, at the constant
        private static List<(Operand Low, Operand High, int Offset)> BaseCandidates(Operand low, Operand high)
        {
            List<(Operand, Operand, int)> list = [(low, high, 0)];

            if (low.AsgOp is Operation add && add.Inst == Instruction.Add && add.SourcesCount == 2 &&
                high.AsgOp is Operation highAdd && highAdd.Inst == Instruction.Add && highAdd.SourcesCount == 2)
            {
                Operand term = add.GetSource(1).Type == OperandType.Constant ? add.GetSource(0) : add.GetSource(0).Type == OperandType.Constant ? add.GetSource(1) : null;
                Operand constant = term == add.GetSource(0) ? add.GetSource(1) : add.GetSource(0);

                if (term != null && constant.Type == OperandType.Constant)
                {
                    for (int index = 0; index < 2; index++)
                    {
                        Operand carry = highAdd.GetSource(index);

                        if (carry.AsgOp is Operation select && select.Inst == Instruction.ConditionalSelect &&
                            select.GetSource(0).AsgOp is Operation compare && compare.SourcesCount == 2 && compare.GetSource(0) == low && compare.GetSource(1) == term)
                        {
                            list.Add((term, highAdd.GetSource(1 - index), constant.Value));
                            break;
                        }
                    }
                }
            }

            return list;
        }

        private static uint PackCbSlotAndOffset(int cbSlot, int cbOffset)
        {
            return (uint)((ushort)cbSlot | ((ushort)cbOffset << 16));
        }

        private static (int, int) UnpackCbSlotAndOffset(uint packed)
        {
            return ((ushort)packed, (ushort)(packed >> 16));
        }

        private static string GetFunctionName(Operation baseOp, bool isMultiTarget, IReadOnlyList<uint> targetCbs)
        {
            StringBuilder nameBuilder = new();
            nameBuilder.Append(baseOp.Inst.ToString());

            nameBuilder.Append(baseOp.StorageKind switch
            {
                StorageKind.GlobalMemoryS8 => "S8",
                StorageKind.GlobalMemoryS16 => "S16",
                StorageKind.GlobalMemoryU8 => "U8",
                StorageKind.GlobalMemoryU16 => "U16",
                _ => string.Empty,
            });

            if (isMultiTarget)
            {
                nameBuilder.Append("Multi");
            }

            foreach (uint targetCb in targetCbs)
            {
                (int sbCbSlot, int sbCbOffset) = UnpackCbSlotAndOffset(targetCb);

                nameBuilder.Append($"_c{sbCbSlot}o{sbCbOffset}");
            }

            return nameBuilder.ToString();
        }

        private static bool TryGenerateStorageOp(
            ResourceManager resourceManager,
            TargetLanguage targetLanguage,
            EmitterContext context,
            Instruction inst,
            StorageKind storageKind,
            Operand offset,
            Operand compare,
            Operand value,
            SearchResult result,
            out Operand resultValue)
        {
            resultValue = null;
            bool isStore = inst.IsAtomic() || inst == Instruction.Store;

            if (!resourceManager.TryGetStorageBufferBinding(result.SbCbSlot, result.SbCbOffset, isStore, out int binding))
            {
                return false;
            }

            Operand wordOffset = context.ShiftRightU32(offset, Const(2));

            if (inst.IsAtomic())
            {
                if (IsSmallInt(storageKind))
                {
                    throw new NotImplementedException();
                }

                switch (inst)
                {
                    case Instruction.AtomicAdd:
                        resultValue = context.AtomicAdd(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicAnd:
                        resultValue = context.AtomicAnd(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicCompareAndSwap:
                        resultValue = context.AtomicCompareAndSwap(StorageKind.StorageBuffer, binding, Const(0), wordOffset, compare, value);
                        break;
                    case Instruction.AtomicMaxS32:
                        if (targetLanguage == TargetLanguage.Spirv)
                        {
                            resultValue = context.AtomicMaxS32(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        }
                        else
                        {
                            resultValue = GenerateAtomicCasLoop(context, wordOffset, binding, (memValue) =>
                            {
                                return context.IMaximumS32(memValue, value);
                            });
                        }

                        break;
                    case Instruction.AtomicMaxU32:
                        resultValue = context.AtomicMaxU32(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicMinS32:
                        if (targetLanguage == TargetLanguage.Spirv)
                        {
                            resultValue = context.AtomicMinS32(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        }
                        else
                        {
                            resultValue = GenerateAtomicCasLoop(context, wordOffset, binding, (memValue) =>
                            {
                                return context.IMinimumS32(memValue, value);
                            });
                        }

                        break;
                    case Instruction.AtomicMinU32:
                        resultValue = context.AtomicMinU32(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicOr:
                        resultValue = context.AtomicOr(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicSwap:
                        resultValue = context.AtomicSwap(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicXor:
                        resultValue = context.AtomicXor(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                }
            }
            else if (inst == Instruction.Store)
            {
                int bitSize = storageKind switch
                {
                    StorageKind.GlobalMemoryS8 or
                    StorageKind.GlobalMemoryU8 => 8,
                    StorageKind.GlobalMemoryS16 or
                    StorageKind.GlobalMemoryU16 => 16,
                    _ => 32,
                };

                if (bitSize < 32)
                {
                    Operand bitOffset = HelperFunctionManager.GetBitOffset(context, offset);

                    GenerateAtomicCasLoop(context, wordOffset, binding, (memValue) =>
                    {
                        return context.BitfieldInsert(memValue, value, bitOffset, Const(bitSize));
                    });
                }
                else
                {
                    context.Store(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                }
            }
            else
            {
                value = context.Load(StorageKind.StorageBuffer, binding, Const(0), wordOffset);

                if (IsSmallInt(storageKind))
                {
                    Operand bitOffset = HelperFunctionManager.GetBitOffset(context, offset);

                    switch (storageKind)
                    {
                        case StorageKind.GlobalMemoryS8:
                            value = context.ShiftRightS32(value, bitOffset);
                            value = context.BitfieldExtractS32(value, Const(0), Const(8));
                            break;
                        case StorageKind.GlobalMemoryS16:
                            value = context.ShiftRightS32(value, bitOffset);
                            value = context.BitfieldExtractS32(value, Const(0), Const(16));
                            break;
                        case StorageKind.GlobalMemoryU8:
                            value = context.ShiftRightU32(value, bitOffset);
                            value = context.BitwiseAnd(value, Const(byte.MaxValue));
                            break;
                        case StorageKind.GlobalMemoryU16:
                            value = context.ShiftRightU32(value, bitOffset);
                            value = context.BitwiseAnd(value, Const(ushort.MaxValue));
                            break;
                    }
                }

                resultValue = value;
            }

            return true;
        }

        private static Operand GenerateAtomicCasLoop(EmitterContext context, Operand wordOffset, int binding, Func<Operand, Operand> opCallback)
        {
            Operand lblLoopHead = Label();

            context.MarkLabel(lblLoopHead);

            Operand oldValue = context.Load(StorageKind.StorageBuffer, binding, Const(0), wordOffset);
            Operand newValue = opCallback(oldValue);

            Operand casResult = context.AtomicCompareAndSwap(
                StorageKind.StorageBuffer,
                binding,
                Const(0),
                wordOffset,
                oldValue,
                newValue);

            Operand casFail = context.ICompareNotEqual(casResult, oldValue);

            context.BranchIfTrue(lblLoopHead, casFail);

            return oldValue;
        }

        private static SearchResult FindUniqueBaseAddressCb(GtsContext gtsContext, BasicBlock block, Operand globalAddress, bool needsOffset)
        {
            globalAddress = Utils.FindLastOperation(globalAddress, block);

            if (globalAddress.Type == OperandType.ConstantBuffer)
            {
                return GetBaseAddressCbWithOffset(globalAddress, Const(0), 0);
            }

            Operation operation = globalAddress.AsgOp as Operation;

            if (operation == null || operation.Inst != Instruction.Add)
            {
                return FindBaseAddressCbFromMemory(gtsContext, operation, 0, needsOffset);
            }

            Operand src1 = operation.GetSource(0);
            Operand src2 = operation.GetSource(1);

            int constOffset = 0;

            if ((src1.Type == OperandType.LocalVariable && src2.Type == OperandType.Constant) ||
                (src2.Type == OperandType.LocalVariable && src1.Type == OperandType.Constant))
            {
                Operand baseAddr;
                Operand offset;

                if (src1.Type == OperandType.LocalVariable)
                {
                    baseAddr = Utils.FindLastOperation(src1, block);
                    offset = src2;
                }
                else
                {
                    baseAddr = Utils.FindLastOperation(src2, block);
                    offset = src1;
                }

                SearchResult result = GetBaseAddressCbWithOffset(baseAddr, offset, 0);
                if (result.Found)
                {
                    return result;
                }

                constOffset = offset.Value;
                operation = baseAddr.AsgOp as Operation;

                if (operation == null || operation.Inst != Instruction.Add)
                {
                    return FindBaseAddressCbFromMemory(gtsContext, operation, constOffset, needsOffset);
                }
            }

            src1 = operation.GetSource(0);
            src2 = operation.GetSource(1);

            // If we have two possible results, we give preference to the ones from
            // the driver reserved constant buffer, as those are the ones that
            // contains the base address.

            // If both are constant buffer, give preference to the second operand,
            // because constant buffer are always encoded as the second operand,
            // so the second operand will always be the one from the last instruction.
            // Unless only the first is from the driver reserved buffer: Ampere code adds uniform registers
            // (Donkey Kong Bananza's effect compute shaders: c0 storage entry + offset from c1, by UIMAD.WIDE),
            // and the IR keeps the order of their sources.

            if (src1.Type == OperandType.ConstantBuffer && src2.Type == OperandType.ConstantBuffer &&
                src1.GetCbufSlot() == DriverReservedCb && src2.GetCbufSlot() != DriverReservedCb)
            {
                return GetBaseAddressCbWithOffset(src1, src2, constOffset);
            }

            if (src1.Type != OperandType.ConstantBuffer ||
                (src1.Type == OperandType.ConstantBuffer && src2.Type == OperandType.ConstantBuffer) ||
                (src2.Type == OperandType.ConstantBuffer && src2.GetCbufSlot() == DriverReservedCb))
            {
                return GetBaseAddressCbWithOffset(src2, src1, constOffset);
            }

            return GetBaseAddressCbWithOffset(src1, src2, constOffset);
        }

        private static SearchResult FindBaseAddressCbFromMemory(GtsContext gtsContext, Operation operation, int constOffset, bool needsOffset)
        {
            if (operation != null)
            {
                if (TryGetMemoryOffsets(operation, out LsMemoryType type, out Operand bo, out int co) &&
                    gtsContext.TryGetMemoryTargetCb(type, bo, co, out SearchResult result) &&
                    (result.Offset != null || !needsOffset))
                {
                    if (constOffset != 0)
                    {
                        return new SearchResult(
                            result.SbCbSlot,
                            result.SbCbOffset,
                            result.Offset,
                            result.ConstOffset + constOffset);
                    }

                    return result;
                }
            }

            return SearchResult.NotFound;
        }

        private static SearchResult GetBaseAddressCbWithOffset(Operand baseAddress, Operand offset, int constOffset)
        {
            if (baseAddress.Type == OperandType.ConstantBuffer)
            {
                int sbCbSlot = baseAddress.GetCbufSlot();
                int sbCbOffset = baseAddress.GetCbufOffset();

                // We require the offset to be aligned to 1 word (64 bits),
                // since the address size is 64-bit and the GPU only supports aligned memory access.
                if ((sbCbOffset & 1) == 0)
                {
                    return new SearchResult(sbCbSlot, sbCbOffset, offset, constOffset);
                }
            }

            return SearchResult.NotFound;
        }

        private static bool TryGetMemoryOffsets(Operation operation, out LsMemoryType type, out Operand baseOffset, out int constOffset)
        {
            baseOffset = null;

            if (operation.Inst is Instruction.Load or Instruction.Store)
            {
                if (operation.StorageKind == StorageKind.SharedMemory)
                {
                    type = LsMemoryType.Shared;
                    return TryGetSharedMemoryOffsets(operation, out baseOffset, out constOffset);
                }
                else if (operation.StorageKind == StorageKind.LocalMemory)
                {
                    type = LsMemoryType.Local;
                    return TryGetLocalMemoryOffset(operation, out constOffset);
                }
            }

            type = default;
            constOffset = 0;
            return false;
        }

        private static bool TryGetSharedMemoryOffsets(Operation operation, out Operand baseOffset, out int constOffset)
        {
            baseOffset = null;
            constOffset = 0;

            // The byte offset is right shifted by 2 to get the 32-bit word offset,
            // so we want to get the byte offset back, since each one of those word
            // offsets are a new "local variable" which will not match.

            if (operation.GetSource(1).AsgOp is Operation shiftRightOp &&
                shiftRightOp.Inst == Instruction.ShiftRightU32 &&
                shiftRightOp.GetSource(1).Type == OperandType.Constant &&
                shiftRightOp.GetSource(1).Value == 2)
            {
                baseOffset = shiftRightOp.GetSource(0);
            }

            // Check if we have a constant offset being added to the base offset.

            if (baseOffset?.AsgOp is Operation addOp && addOp.Inst == Instruction.Add)
            {
                Operand src1 = addOp.GetSource(0);
                Operand src2 = addOp.GetSource(1);

                if (src1.Type == OperandType.Constant && src2.Type == OperandType.LocalVariable)
                {
                    constOffset = src1.Value;
                    baseOffset = src2;
                }
                else if (src1.Type == OperandType.LocalVariable && src2.Type == OperandType.Constant)
                {
                    baseOffset = src1;
                    constOffset = src2.Value;
                }
            }

            return baseOffset != null && baseOffset.Type == OperandType.LocalVariable;
        }

        private static bool TryGetLocalMemoryOffset(Operation operation, out int constOffset)
        {
            Operand offset = operation.GetSource(1);

            if (offset.Type == OperandType.Constant)
            {
                constOffset = offset.Value;
                return true;
            }

            constOffset = 0;
            return false;
        }
    }
}
