using Ryujinx.Graphics.Shader.Translation;

namespace Ryujinx.Graphics.Shader.StructuredIr
{
    readonly struct MemoryDefinition
    {
        public string Name { get; }
        public AggregateType Type { get; }
        public int ArrayLength { get; }
        public bool Zeroed { get; }   // nxnative: declared with a zero initializer (a private memory read before the shader writes it: GlobalToStorage's pool cache)

        public MemoryDefinition(string name, AggregateType type, int arrayLength = 1, bool zeroed = false)
        {
            Name = name;
            Type = type;
            ArrayLength = arrayLength;
            Zeroed = zeroed;
        }
    }
}
