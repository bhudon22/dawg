# DAWG Builder

Builds a compressed **DAWG** (Directed Acyclic Word Graph) from a plain-text word list and serialises it to a compact binary format.

Implemented twice — once in **C** (`main.c`) and once in **C#** (`DawgCs/`) — producing byte-identical output.

## What is a DAWG?

A DAWG is a trie that has been compressed by merging all nodes whose subtrees are structurally identical. Because English suffixes repeat heavily (*-ing*, *-tion*, *-ness*, …), the compression is dramatic:

| Stage | Nodes |
|---|---|
| Trie (before compression) | 1,027,810 |
| DAWG (after compression) | 157,183 |
| **Reduction** | **84.7 %** |

The resulting structure encodes 370,105 dictionary words in **1,481,652 bytes** (~1.4 MB).

## Binary format

The output file (`dawg.bin`) is a flat array of `uint32` values — no header. Each entry encodes one edge:

```
Bits  0– 4   character  (1='a' … 26='z')
Bit   5      end-of-word flag  (this edge completes a valid word)
Bit   6      end-of-node flag  (last sibling in the current list)
Bits  7–31   next pointer      (index of the child's sibling list; 0 = leaf)
```

The root node's children start at index 0.
See [`dawg_format.txt`](dawg_format.txt) for the full spec with traversal examples.

## Building and running

### C (CMake / CLion)

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug
./cmake-build-debug/dawg words.txt
```

### C# (.NET 9)

```bash
cd DawgCs
dotnet run -c Release -- ../words.txt dawg_cs.bin
```

Both programs accept an optional word-list path as the first argument and output path as the second. They print the same stats and verify the binary by walking it and counting words.

## Word list

`words.txt` contains ~370K lowercase English words, one per line. Words with non-alphabetic characters are skipped automatically.

`testwords.txt` is a 12-word subset useful for quick tests and DOT graph visualisation.
