#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#define ALPHABET_SIZE 26

typedef struct TrieNode {
    struct TrieNode *children[ALPHABET_SIZE];
    int child_is_terminal[ALPHABET_SIZE]; /* end-of-word flag per edge, not per node */
    int is_end_of_word; /* used during trie construction, then moved to edges */
    int visited;        /* flag to avoid reprocessing in DAWG */
} TrieNode;

TrieNode *trie_create_node(void) {
    TrieNode *node = calloc(1, sizeof(TrieNode));
    if (!node) {
        fprintf(stderr, "Failed to allocate trie node\n");
        exit(1);
    }
    return node;
}

void trie_insert(TrieNode *root, const char *word) {
    TrieNode *current = root;
    for (int i = 0; word[i] != '\0'; i++) {
        int index = word[i] - 'a';
        if (!current->children[index]) {
            current->children[index] = trie_create_node();
        }
        current = current->children[index];
    }
    current->is_end_of_word = 1;
}

/* Count trie nodes (before compression, no sharing) */
int trie_count_nodes(TrieNode *node) {
    if (!node) return 0;
    int count = 1;
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        count += trie_count_nodes(node->children[i]);
    }
    return count;
}

/* Count unique nodes in DAWG using visited flag */
int dawg_count_nodes(TrieNode *node) {
    if (!node || node->visited) return 0;
    node->visited = 1;
    int count = 1;
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        count += dawg_count_nodes(node->children[i]);
    }
    return count;
}

/* Reset visited flags */
void dawg_reset_visited(TrieNode *node) {
    if (!node || !node->visited) return;
    node->visited = 0;
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        dawg_reset_visited(node->children[i]);
    }
}

/* Count words by walking all paths (no dedup — shared nodes are visited per path) */
void dawg_count_words(TrieNode *node, int *word_count) {
    if (!node) return;
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (node->children[i]) {
            if (node->child_is_terminal[i]) (*word_count)++;
            dawg_count_words(node->children[i], word_count);
        }
    }
}

/* Print all words in the DAWG (using edge-based terminal flags) */
void dawg_print(TrieNode *node, char *buffer, int depth) {
    if (!node) return;
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (node->children[i]) {
            buffer[depth] = 'a' + i;
            if (node->child_is_terminal[i]) {
                buffer[depth + 1] = '\0';
                printf("  %s\n", buffer);
            }
            dawg_print(node->children[i], buffer, depth + 1);
        }
    }
}

/*
 * Move is_end_of_word from nodes to parent edges (child_is_terminal).
 */
void trie_move_eow_to_edges(TrieNode *node) {
    if (!node) return;
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (node->children[i]) {
            node->child_is_terminal[i] = node->children[i]->is_end_of_word;
            trie_move_eow_to_edges(node->children[i]);
        }
    }
}

/* ---- DAWG compression via bottom-up signature matching ---- */

#define HASH_TABLE_SIZE 262147 /* larger prime for big word lists */

typedef struct HashEntry {
    TrieNode *canonical;
    TrieNode *children_key[ALPHABET_SIZE];
    int terminal_key[ALPHABET_SIZE]; /* child_is_terminal is part of the signature */
    struct HashEntry *next;
} HashEntry;

typedef struct {
    HashEntry *buckets[HASH_TABLE_SIZE];
} HashMap;

HashMap *hashmap_create(void) {
    HashMap *map = calloc(1, sizeof(HashMap));
    if (!map) {
        fprintf(stderr, "Failed to allocate hash map\n");
        exit(1);
    }
    return map;
}

void hashmap_free(HashMap *map) {
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        HashEntry *e = map->buckets[i];
        while (e) {
            HashEntry *next = e->next;
            free(e);
            e = next;
        }
    }
    free(map);
}

unsigned int hash_node_sig(TrieNode *node) {
    unsigned int h = 0;
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        h = h * 131 + (unsigned int)((size_t)node->children[i] >> 4);
        h = h * 31 + (unsigned int)node->child_is_terminal[i];
    }
    return h % HASH_TABLE_SIZE;
}

TrieNode *hashmap_find_or_insert(HashMap *map, TrieNode *node) {
    unsigned int h = hash_node_sig(node);
    HashEntry *e = map->buckets[h];
    while (e) {
        if (memcmp(e->children_key, node->children, sizeof(node->children)) == 0 &&
            memcmp(e->terminal_key, node->child_is_terminal, sizeof(node->child_is_terminal)) == 0) {
            return e->canonical;
        }
        e = e->next;
    }
    e = malloc(sizeof(HashEntry));
    memcpy(e->children_key, node->children, sizeof(node->children));
    memcpy(e->terminal_key, node->child_is_terminal, sizeof(node->child_is_terminal));
    e->canonical = node;
    e->next = map->buckets[h];
    map->buckets[h] = e;
    return node;
}

TrieNode *dawg_compress_node(TrieNode *node, HashMap *map) {
    if (!node) return NULL;

    /* Avoid reprocessing nodes already canonicalized */
    if (node->visited) return node;
    node->visited = 1;

    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (node->children[i]) {
            node->children[i] = dawg_compress_node(node->children[i], map);
        }
    }

    return hashmap_find_or_insert(map, node);
}

void dawg_compress(TrieNode *root, HashMap *map) {
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (root->children[i]) {
            root->children[i] = dawg_compress_node(root->children[i], map);
        }
    }
}

/* ---- Graphviz DOT visualization ---- */

int dawg_get_node_id(TrieNode *node, TrieNode **seen, int *seen_count) {
    for (int i = 0; i < *seen_count; i++) {
        if (seen[i] == node) return i;
    }
    seen[(*seen_count)] = node;
    return (*seen_count)++;
}

void dawg_write_dot_edges(FILE *fp, TrieNode *node, TrieNode **seen, int *seen_count) {
    if (!node) return;
    int parent_id = dawg_get_node_id(node, seen, seen_count);
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (node->children[i]) {
            int child_id = dawg_get_node_id(node->children[i], seen, seen_count);
            char letter = 'a' + i;
            if (node->child_is_terminal[i]) {
                fprintf(fp, "  n%d -> n%d [label=\"%c\" color=green fontcolor=green penwidth=2.0];\n",
                        parent_id, child_id, letter);
            } else {
                fprintf(fp, "  n%d -> n%d [label=\"%c\"];\n",
                        parent_id, child_id, letter);
            }
        }
    }
}

void dawg_export_dot(TrieNode *root, const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, "Could not open DOT file: %s\n", filename);
        return;
    }

    fprintf(fp, "digraph DAWG {\n");
    fprintf(fp, "  rankdir=TB;\n");
    fprintf(fp, "  node [shape=circle width=0.3 fontsize=10];\n");
    fprintf(fp, "  edge [fontsize=12];\n");
    fprintf(fp, "  n0 [label=\"\" shape=doublecircle];\n");

    TrieNode *seen[1000];
    int seen_count = 0;
    dawg_get_node_id(root, seen, &seen_count);

    TrieNode *queue[1000];
    int queue_front = 0, queue_back = 0;
    queue[queue_back++] = root;
    TrieNode *visited[1000];
    int visited_count = 0;
    visited[visited_count++] = root;

    while (queue_front < queue_back) {
        TrieNode *node = queue[queue_front++];
        for (int i = 0; i < ALPHABET_SIZE; i++) {
            if (node->children[i]) {
                int found = 0;
                for (int j = 0; j < visited_count; j++) {
                    if (visited[j] == node->children[i]) { found = 1; break; }
                }
                if (!found) {
                    dawg_get_node_id(node->children[i], seen, &seen_count);
                    visited[visited_count++] = node->children[i];
                    queue[queue_back++] = node->children[i];
                }
            }
        }
    }

    for (int i = 1; i < seen_count; i++) {
        fprintf(fp, "  n%d [label=\"%d\"];\n", i, i);
    }
    for (int i = 0; i < visited_count; i++) {
        dawg_write_dot_edges(fp, visited[i], seen, &seen_count);
    }

    fprintf(fp, "}\n");
    fclose(fp);
    printf("DOT file written to: %s\n", filename);
}

/* ---- Flatten DAWG into packed uint32_t array ---- */

/*
 * Binary format per uint32_t entry:
 *   Bits 0-4:  Character (5 bits, 'a'-'z' = 1-26, 0 = null)
 *   Bit  5:    End of Word (terminal edge)
 *   Bit  6:    End of Node (last sibling in child list)
 *   Bits 7-31: Next Pointer (25 bits, index of first child of target node)
 */

#define PACK_CHAR(c)    ((uint32_t)(c) & 0x1F)
#define PACK_EOW(eow)   (((uint32_t)(eow) & 1) << 5)
#define PACK_EON(eon)    (((uint32_t)(eon) & 1) << 6)
#define PACK_NEXT(ptr)   (((uint32_t)(ptr) & 0x1FFFFFF) << 7)

#define UNPACK_CHAR(v)   ((v) & 0x1F)
#define UNPACK_EOW(v)    (((v) >> 5) & 1)
#define UNPACK_EON(v)    (((v) >> 6) & 1)
#define UNPACK_NEXT(v)   (((v) >> 7) & 0x1FFFFFF)

typedef struct {
    uint32_t *data;
    int size;
    int capacity;
} PackedDAWG;

PackedDAWG *packed_dawg_create(int initial_capacity) {
    PackedDAWG *pd = malloc(sizeof(PackedDAWG));
    pd->data = calloc(initial_capacity, sizeof(uint32_t));
    pd->size = 0;
    pd->capacity = initial_capacity;
    return pd;
}

void packed_dawg_free(PackedDAWG *pd) {
    free(pd->data);
    free(pd);
}

int count_children(TrieNode *node) {
    int count = 0;
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (node->children[i]) count++;
    }
    return count;
}

/*
 * Hash map for node pointer -> array offset (used during flattening).
 * Much faster than linear scan for large DAWGs.
 */
#define OFFSET_MAP_SIZE 262147

typedef struct OffsetEntry {
    TrieNode *node;
    int offset;
    struct OffsetEntry *next;
} OffsetEntry;

typedef struct {
    OffsetEntry *buckets[OFFSET_MAP_SIZE];
} OffsetMap;

OffsetMap *offset_map_create(void) {
    return calloc(1, sizeof(OffsetMap));
}

void offset_map_free(OffsetMap *om) {
    for (int i = 0; i < OFFSET_MAP_SIZE; i++) {
        OffsetEntry *e = om->buckets[i];
        while (e) {
            OffsetEntry *next = e->next;
            free(e);
            e = next;
        }
    }
    free(om);
}

unsigned int hash_ptr(TrieNode *ptr) {
    return (unsigned int)((size_t)ptr >> 4) % OFFSET_MAP_SIZE;
}

int offset_map_get(OffsetMap *om, TrieNode *node) {
    unsigned int h = hash_ptr(node);
    OffsetEntry *e = om->buckets[h];
    while (e) {
        if (e->node == node) return e->offset;
        e = e->next;
    }
    return -1;
}

void offset_map_put(OffsetMap *om, TrieNode *node, int offset) {
    unsigned int h = hash_ptr(node);
    OffsetEntry *e = malloc(sizeof(OffsetEntry));
    e->node = node;
    e->offset = offset;
    e->next = om->buckets[h];
    om->buckets[h] = e;
}

/*
 * Flatten the DAWG into a packed uint32_t array.
 * Uses BFS with hash map for O(1) node-to-offset lookups.
 */
PackedDAWG *dawg_flatten(TrieNode *root) {
    PackedDAWG *pd = packed_dawg_create(500000);
    OffsetMap *om = offset_map_create();

    /* BFS queue */
    int queue_cap = 500000;
    TrieNode **queue = malloc(queue_cap * sizeof(TrieNode *));
    int q_front = 0, q_back = 0;

    /* Root's children start at index 0 */
    int root_child_count = count_children(root);
    offset_map_put(om, root, 0);
    pd->size = root_child_count;
    queue[q_back++] = root;

    /* Phase 1: BFS to assign offsets */
    while (q_front < q_back) {
        TrieNode *node = queue[q_front++];

        for (int i = 0; i < ALPHABET_SIZE; i++) {
            if (node->children[i]) {
                TrieNode *child = node->children[i];

                if (offset_map_get(om, child) == -1) {
                    int cc = count_children(child);
                    if (cc > 0) {
                        offset_map_put(om, child, pd->size);
                        pd->size += cc;
                        if (q_back >= queue_cap) {
                            queue_cap *= 2;
                            queue = realloc(queue, queue_cap * sizeof(TrieNode *));
                        }
                        queue[q_back++] = child;
                    } else {
                        offset_map_put(om, child, 0); /* leaf */
                    }
                }
            }
        }
    }

    /* Ensure packed array is large enough */
    if (pd->size > pd->capacity) {
        pd->capacity = pd->size * 2;
        pd->data = realloc(pd->data, pd->capacity * sizeof(uint32_t));
        memset(pd->data, 0, pd->capacity * sizeof(uint32_t));
    }

    /* Phase 2: BFS again to fill in entries */
    q_front = 0;
    q_back = 0;
    queue[q_back++] = root;

    /* Track visited for phase 2 */
    OffsetMap *visited = offset_map_create();
    offset_map_put(visited, root, 1);

    while (q_front < q_back) {
        TrieNode *node = queue[q_front++];
        int base_offset = offset_map_get(om, node);
        int slot = 0;
        int num_children = count_children(node);

        for (int i = 0; i < ALPHABET_SIZE; i++) {
            if (node->children[i]) {
                TrieNode *child = node->children[i];
                int child_offset = offset_map_get(om, child);
                int is_last = (slot == num_children - 1);

                uint32_t entry = 0;
                entry |= PACK_CHAR(i + 1);
                entry |= PACK_EOW(node->child_is_terminal[i]);
                entry |= PACK_EON(is_last);
                entry |= PACK_NEXT(child_offset);

                pd->data[base_offset + slot] = entry;
                slot++;

                if (offset_map_get(visited, child) == -1 && count_children(child) > 0) {
                    offset_map_put(visited, child, 1);
                    queue[q_back++] = child;
                }
            }
        }
    }

    free(queue);
    offset_map_free(om);
    offset_map_free(visited);

    return pd;
}

/* Print the packed array for debugging */
void packed_dawg_dump(PackedDAWG *pd) {
    printf("Packed DAWG: %d entries (%d bytes)\n", pd->size, (int)(pd->size * sizeof(uint32_t)));
    printf("%-6s %-6s %-5s %-5s %-6s\n", "Index", "Char", "EOW", "EON", "Next");
    for (int i = 0; i < pd->size; i++) {
        uint32_t v = pd->data[i];
        int c = UNPACK_CHAR(v);
        int eow = UNPACK_EOW(v);
        int eon = UNPACK_EON(v);
        int next = UNPACK_NEXT(v);
        printf("%-6d %-6c %-5d %-5d %-6d\n", i, c ? 'a' + c - 1 : '.', eow, eon, next);
    }
}

/* ---- Binary DAWG reader and verifier ---- */

void packed_dawg_walk(uint32_t *data, int index, char *buffer, int depth,
                      int *word_count, int print_words) {
    if (index == 0 && depth > 0) return;

    while (1) {
        uint32_t v = data[index];
        int c = UNPACK_CHAR(v);
        int eow = UNPACK_EOW(v);
        int eon = UNPACK_EON(v);
        int next = UNPACK_NEXT(v);

        buffer[depth] = 'a' + c - 1;

        if (eow) {
            (*word_count)++;
            if (print_words) {
                buffer[depth + 1] = '\0';
                printf("  %s\n", buffer);
            }
        }

        if (next != 0) {
            packed_dawg_walk(data, next, buffer, depth + 1, word_count, print_words);
        }

        if (eon) break;
        index++;
    }
}

void packed_dawg_verify(const char *filename, int print_words) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "Could not open binary file: %s\n", filename);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    int entry_count = file_size / sizeof(uint32_t);
    uint32_t *data = malloc(file_size);
    fread(data, sizeof(uint32_t), entry_count, fp);
    fclose(fp);

    printf("\n--- Verifying binary file: %s ---\n", filename);
    printf("File size: %ld bytes (%d entries)\n", file_size, entry_count);

    char buffer[256];
    int word_count = 0;
    packed_dawg_walk(data, 0, buffer, 0, &word_count, print_words);

    printf("Words found in binary: %d\n", word_count);

    free(data);
}

/* ---- File loading ---- */

/*
 * Check if a word is clean: only lowercase a-z, no digits, hyphens, etc.
 * Converts to lowercase in place. Returns 1 if clean, 0 if not.
 */
int clean_word(char *word) {
    for (int i = 0; word[i] != '\0'; i++) {
        if (word[i] >= 'A' && word[i] <= 'Z') {
            word[i] = word[i] - 'A' + 'a';
        } else if (word[i] < 'a' || word[i] > 'z') {
            return 0; /* reject: contains non-alpha character */
        }
    }
    return 1;
}

TrieNode *trie_load_from_file(const char *filename, int *words_loaded, int *words_skipped) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Could not open file: %s\n", filename);
        return NULL;
    }

    TrieNode *root = trie_create_node();
    char line[256];
    *words_loaded = 0;
    *words_skipped = 0;

    while (fgets(line, sizeof(line), fp)) {
        int len = strlen(line);
        while (len > 0 && isspace(line[len - 1])) {
            line[--len] = '\0';
        }
        if (len > 0) {
            if (clean_word(line)) {
                trie_insert(root, line);
                (*words_loaded)++;
            } else {
                (*words_skipped)++;
            }
        }
    }

    fclose(fp);
    return root;
}

/* ---- Main ---- */

int main(int argc, char *argv[]) {
    const char *filename = "words.txt";
    if (argc > 1) {
        filename = argv[1];
    }

    printf("Loading words from: %s\n", filename);

    int words_loaded = 0, words_skipped = 0;
    TrieNode *root = trie_load_from_file(filename, &words_loaded, &words_skipped);
    if (!root) return 1;

    printf("Words loaded: %d\n", words_loaded);
    if (words_skipped > 0) {
        printf("Words skipped (non-alpha): %d\n", words_skipped);
    }
    printf("\n");

    /* Trie stats before compression */
    int trie_nodes = trie_count_nodes(root);
    printf("--- Before compression ---\n");
    printf("Trie nodes: %d\n\n", trie_nodes);

    /* Move end-of-word flags from nodes to edges */
    trie_move_eow_to_edges(root);

    /* Compress trie into DAWG */
    printf("Compressing...\n");
    HashMap *map = hashmap_create();
    dawg_compress(root, map);

    /* Stats after compression — root was not processed by compress_node,
       so force its visited flag before resetting the tree */
    root->visited = 1;
    dawg_reset_visited(root);
    int dawg_nodes = dawg_count_nodes(root);

    int word_count = 0;
    dawg_count_words(root, &word_count);

    printf("\n--- After compression ---\n");
    printf("DAWG nodes: %d\n", dawg_nodes);
    printf("Words in DAWG: %d\n", word_count);
    printf("Compression: %d -> %d nodes (%.1f%% reduction)\n\n",
           trie_nodes, dawg_nodes,
           100.0 * (1.0 - (double)dawg_nodes / trie_nodes));

    /* Export visualization (small datasets only) */
    if (dawg_nodes <= 100) {
        dawg_export_dot(root, "dawg.dot");
        printf("Words stored:\n");
        char buffer[256];
        dawg_print(root, buffer, 0);
    }

    /* Flatten DAWG into packed uint32_t array */
    printf("--- Flattening DAWG ---\n");
    PackedDAWG *pd = dawg_flatten(root);
    if (pd->size <= 100) {
        packed_dawg_dump(pd);
    } else {
        printf("Packed DAWG: %d entries (%d bytes)\n", pd->size, (int)(pd->size * sizeof(uint32_t)));
    }

    /* Write binary file */
    const char *outfile = "dawg.bin";
    FILE *fp = fopen(outfile, "wb");
    if (!fp) {
        fprintf(stderr, "Could not open output file: %s\n", outfile);
        return 1;
    }
    fwrite(pd->data, sizeof(uint32_t), pd->size, fp);
    fclose(fp);
    printf("\nBinary file written: %s (%d bytes)\n", outfile, (int)(pd->size * sizeof(uint32_t)));

    /* Verify by reading back the binary */
    packed_dawg_verify(outfile, words_loaded <= 100);

    packed_dawg_free(pd);
    hashmap_free(map);
    return 0;
}
