using System.Runtime.CompilerServices;

// ─── Entry point ───────────────────────────────────────────────────────────────
// Top-level statements must precede type declarations in C#.

string wordsFile = args.Length > 0 ? args[0] : "words.txt";
string outFile   = args.Length > 1 ? args[1] : "dawg_cs.bin";

if (!File.Exists(wordsFile))
{
    Console.Error.WriteLine($"Error: could not open file: {wordsFile}");
    return 1;
}

Console.WriteLine($"Loading words from: {wordsFile}");
var root = DawgBuilder.LoadTrie(wordsFile, out int loaded, out int skipped);
Console.WriteLine($"Words loaded: {loaded}");
if (skipped > 0) Console.WriteLine($"Words skipped (non-alpha): {skipped}");
Console.WriteLine();

int trieNodes = DawgBuilder.CountTrieNodes(root);
Console.WriteLine("--- Before compression ---");
Console.WriteLine($"Trie nodes: {trieNodes}");
Console.WriteLine();

DawgBuilder.MoveEowToEdges(root);

Console.WriteLine("Compressing...");
DawgBuilder.Compress(root);

int dawgNodes = DawgBuilder.CountDawgNodes(root);
int wordCount = DawgBuilder.CountWords(root);
double reduction = 100.0 * (1.0 - (double)dawgNodes / trieNodes);

Console.WriteLine();
Console.WriteLine("--- After compression ---");
Console.WriteLine($"DAWG nodes: {dawgNodes}");
Console.WriteLine($"Words in DAWG: {wordCount}");
Console.WriteLine($"Compression: {trieNodes} -> {dawgNodes} nodes ({reduction:F1}% reduction)");
Console.WriteLine();

Console.WriteLine("--- Flattening DAWG ---");
uint[] data = DawgBuilder.Flatten(root);
Console.WriteLine($"Packed DAWG: {data.Length} entries ({data.Length * 4} bytes)");

// Write binary (BinaryWriter writes little-endian, matching C's fwrite on Windows)
using (var bw = new BinaryWriter(File.Open(outFile, FileMode.Create)))
    foreach (uint v in data)
        bw.Write(v);

Console.WriteLine($"\nBinary file written: {outFile} ({data.Length * 4} bytes)");

// Verify by walking the packed array
Console.WriteLine($"\n--- Verifying binary file: {outFile} ---");
Console.WriteLine($"File size: {data.Length * 4} bytes ({data.Length} entries)");
int verified = DawgBuilder.Verify(data, loaded <= 100);
Console.WriteLine($"Words found in binary: {verified}");

return 0;

// ─── TrieNode ──────────────────────────────────────────────────────────────────
// Mirrors the C struct: 26-child array + per-edge terminal flag + bookkeeping.

class TrieNode
{
    public readonly TrieNode?[] Children       = new TrieNode?[26];
    public readonly bool[]      ChildIsTerminal = new bool[26];
    public bool IsEndOfWord;
    public bool Visited;

    public int CountChildren()
    {
        int n = 0;
        foreach (var c in Children) if (c != null) n++;
        return n;
    }
}

// ─── Node-signature equality comparer ─────────────────────────────────────────
// Two nodes are "equal" when they have the same children (by reference identity)
// and the same per-edge terminal flags — the DAWG compression key.

class NodeSigComparer : IEqualityComparer<TrieNode>
{
    public static readonly NodeSigComparer Instance = new();

    public bool Equals(TrieNode? x, TrieNode? y)
    {
        if (x is null || y is null) return ReferenceEquals(x, y);
        for (int i = 0; i < 26; i++)
        {
            if (!ReferenceEquals(x.Children[i], y.Children[i])) return false;
            if (x.ChildIsTerminal[i] != y.ChildIsTerminal[i])   return false;
        }
        return true;
    }

    public int GetHashCode(TrieNode n)
    {
        // Mirrors the C hash_node_sig(): mix child identity + terminal flags.
        uint h = 0;
        for (int i = 0; i < 26; i++)
        {
            h = h * 131 + (uint)RuntimeHelpers.GetHashCode(n.Children[i]);
            h = h * 31  + (n.ChildIsTerminal[i] ? 1u : 0u);
        }
        return (int)h;
    }
}

// ─── DAWG builder ──────────────────────────────────────────────────────────────
static class DawgBuilder
{
    // ── Trie construction ────────────────────────────────────────────────────

    /// <summary>
    /// Load words.txt, normalise to lowercase, reject non-alpha words,
    /// and insert clean words into a trie.  Mirrors trie_load_from_file().
    /// </summary>
    public static TrieNode LoadTrie(string path, out int loaded, out int skipped)
    {
        var root = new TrieNode();
        loaded = skipped = 0;
        var buf = new char[256];   // reusable buffer; words are at most a few dozen chars

        foreach (var rawLine in File.ReadLines(path))
        {
            var line = rawLine.TrimEnd();           // mirrors isspace() strip in C
            if (line.Length == 0) continue;

            // Convert uppercase → lowercase; reject any non-alpha character.
            bool ok = true;
            for (int i = 0; i < line.Length; i++)
            {
                char c = line[i];
                if      (c is >= 'A' and <= 'Z') c = (char)(c - 'A' + 'a');
                else if (c is < 'a'  or  > 'z') { ok = false; break; }
                buf[i] = c;
            }

            if (ok) { TrieInsert(root, buf.AsSpan(0, line.Length)); loaded++; }
            else    { skipped++; }
        }

        return root;
    }

    static void TrieInsert(TrieNode root, ReadOnlySpan<char> word)
    {
        var node = root;
        foreach (char c in word)
        {
            int i = c - 'a';
            node.Children[i] ??= new TrieNode();
            node = node.Children[i]!;
        }
        node.IsEndOfWord = true;
    }

    /// <summary>Count all nodes in the trie (before compression).</summary>
    public static int CountTrieNodes(TrieNode? node)
    {
        if (node is null) return 0;
        int n = 1;
        foreach (var c in node.Children) n += CountTrieNodes(c);
        return n;
    }

    // ── End-of-word migration ────────────────────────────────────────────────

    /// <summary>
    /// Move is_end_of_word flags from nodes onto their parent edges
    /// (child_is_terminal).  Must be done before compression.
    /// Mirrors trie_move_eow_to_edges().
    /// </summary>
    public static void MoveEowToEdges(TrieNode? node)
    {
        if (node is null) return;
        for (int i = 0; i < 26; i++)
        {
            var child = node.Children[i];
            if (child != null)
            {
                node.ChildIsTerminal[i] = child.IsEndOfWord;
                MoveEowToEdges(child);
            }
        }
    }

    // ── DAWG compression ─────────────────────────────────────────────────────

    /// <summary>
    /// Bottom-up DAWG compression via signature deduplication.
    /// Mirrors dawg_compress() + dawg_compress_node().
    /// </summary>
    public static void Compress(TrieNode root)
    {
        var map = new Dictionary<TrieNode, TrieNode>(NodeSigComparer.Instance);
        for (int i = 0; i < 26; i++)
            if (root.Children[i] != null)
                root.Children[i] = CompressNode(root.Children[i]!, map);
    }

    static TrieNode CompressNode(TrieNode node, Dictionary<TrieNode, TrieNode> map)
    {
        if (node.Visited) return node;   // already canonical — skip
        node.Visited = true;

        // Compress children first so their identities are canonical.
        for (int i = 0; i < 26; i++)
            if (node.Children[i] != null)
                node.Children[i] = CompressNode(node.Children[i]!, map);

        // Look up this node's signature; return existing canonical or register self.
        if (map.TryGetValue(node, out var canonical)) return canonical;
        map[node] = node;
        return node;
    }

    // ── Post-compression stats ───────────────────────────────────────────────

    /// <summary>Count unique nodes in the compressed DAWG.</summary>
    public static int CountDawgNodes(TrieNode root)
    {
        var seen = new HashSet<TrieNode>(ReferenceEqualityComparer.Instance);
        return Count(root, seen);

        static int Count(TrieNode? n, HashSet<TrieNode> seen)
        {
            if (n is null || !seen.Add(n)) return 0;
            int total = 1;
            foreach (var c in n.Children) total += Count(c, seen);
            return total;
        }
    }

    /// <summary>
    /// Count words by walking all edge-paths (shared nodes visited multiple
    /// times, once per distinct prefix path — same as C dawg_count_words).
    /// </summary>
    public static int CountWords(TrieNode? node)
    {
        if (node is null) return 0;
        int n = 0;
        for (int i = 0; i < 26; i++)
        {
            if (node.Children[i] != null)
            {
                if (node.ChildIsTerminal[i]) n++;
                n += CountWords(node.Children[i]);
            }
        }
        return n;
    }

    // ── Flatten into packed uint32 array ────────────────────────────────────

    //  Bit layout per uint32  (mirrors the C macros):
    //    Bits  0-4   char value  1='a' … 26='z'
    //    Bit   5     end-of-word flag
    //    Bit   6     end-of-node flag (last sibling in list)
    //    Bits  7-31  next pointer (index of child's sibling list; 0 = leaf)

    /// <summary>
    /// Flatten the DAWG into a packed uint32 array using BFS.
    /// Mirrors dawg_flatten().
    /// </summary>
    public static uint[] Flatten(TrieNode root)
    {
        var offsets = new Dictionary<TrieNode, int>(ReferenceEqualityComparer.Instance);
        var queue   = new Queue<TrieNode>();

        // ── Phase 1: BFS — assign array offsets ─────────────────────────────
        int totalSize = root.CountChildren();
        offsets[root] = 0;
        queue.Enqueue(root);

        while (queue.Count > 0)
        {
            var node = queue.Dequeue();
            for (int i = 0; i < 26; i++)
            {
                var child = node.Children[i];
                if (child != null && !offsets.ContainsKey(child))
                {
                    int cc = child.CountChildren();
                    if (cc > 0)
                    {
                        offsets[child] = totalSize;
                        totalSize     += cc;
                        queue.Enqueue(child);
                    }
                    else
                    {
                        offsets[child] = 0;   // leaf — next pointer will be 0
                    }
                }
            }
        }

        // ── Phase 2: BFS — fill in entries ──────────────────────────────────
        var data    = new uint[totalSize];
        var visited = new HashSet<TrieNode>(ReferenceEqualityComparer.Instance);
        queue.Clear();
        queue.Enqueue(root);
        visited.Add(root);

        while (queue.Count > 0)
        {
            var node        = queue.Dequeue();
            int baseOff     = offsets[node];
            int numChildren = node.CountChildren();
            int slot        = 0;

            for (int i = 0; i < 26; i++)
            {
                var child = node.Children[i];
                if (child is null) continue;

                int  childOff = offsets[child];
                bool isLast   = (slot == numChildren - 1);

                uint entry = (uint)(i + 1)                          // char (1-26)
                    | ((node.ChildIsTerminal[i] ? 1u : 0u) << 5)    // EOW
                    | ((isLast                  ? 1u : 0u) << 6)    // EON
                    | ((uint)(childOff & 0x1FFFFFF)        << 7);   // next ptr

                data[baseOff + slot] = entry;
                slot++;

                if (child.CountChildren() > 0 && visited.Add(child))
                    queue.Enqueue(child);
            }
        }

        return data;
    }

    // ── Binary verification ──────────────────────────────────────────────────

    /// <summary>Walk the packed array and count (optionally print) all words.</summary>
    public static int Verify(uint[] data, bool printWords)
    {
        var buf   = new char[256];
        int count = 0;
        Walk(data, 0, buf, 0, ref count, printWords);
        return count;
    }

    static void Walk(uint[] data, int idx, char[] buf, int depth,
                     ref int count, bool print)
    {
        if (idx == 0 && depth > 0) return;   // leaf sentinel

        while (true)
        {
            uint v    = data[idx];
            int  c    = (int)(v & 0x1F);
            bool eow  = ((v >> 5) & 1) == 1;
            bool eon  = ((v >> 6) & 1) == 1;
            int  next = (int)((v >> 7) & 0x1FFFFFF);

            buf[depth] = (char)('a' + c - 1);

            if (eow)
            {
                count++;
                if (print) Console.WriteLine("  " + new string(buf, 0, depth + 1));
            }

            if (next != 0)
                Walk(data, next, buf, depth + 1, ref count, print);

            if (eon) break;
            idx++;
        }
    }
}
