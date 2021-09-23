#include "gc-heap-snapshot.h"

#include "julia_internal.h"
#include "gc.h"

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <iostream>

using std::cout; using std::endl;
using std::vector;
using std::string;
using std::unordered_map;
using std::unordered_set;

// TODOs:
// Roots
//   - Correctly reporting the roots to JS somehow..?
//   - Making sure we're actually encoding all the roots.
// Field Names & node types
//   - Bug fixing
// Sizes
//   - string sizes


int gc_heap_snapshot_enabled = 0;

// https://stackoverflow.com/a/33799784/751061
void print_str_escape_json(JL_STREAM *stream, const std::string &s) {
    jl_printf(stream, "\"");
    for (auto c = s.cbegin(); c != s.cend(); c++) {
        switch (*c) {
        case '"': jl_printf(stream, "\\\""); break;
        case '\\': jl_printf(stream, "\\\\"); break;
        case '\b': jl_printf(stream, "\\b"); break;
        case '\f': jl_printf(stream, "\\f"); break;
        case '\n': jl_printf(stream, "\\n"); break;
        case '\r': jl_printf(stream, "\\r"); break;
        case '\t': jl_printf(stream, "\\t"); break;
        default:
            if ('\x00' <= *c && *c <= '\x1f') {
                jl_printf(stream, "\\u%04x", (int)*c);
            } else {
                jl_printf(stream, "%c", *c);
            }
        }
    }
    jl_printf(stream, "\"");
}

struct HeapSnapshot;
void serialize_heap_snapshot(JL_STREAM *stream, HeapSnapshot &snapshot);
static inline void _record_gc_edge(const char *node_type, const char *edge_type,
                                   jl_value_t *a, jl_value_t *b, size_t name_or_index);
void _add_internal_root(HeapSnapshot *snapshot);

// Edges
// "edge_fields":
//   [ "type", "name_or_index", "to_node" ]

struct Edge {
    size_t type; // These *must* match the Enums on the JS side; control interpretation of name_or_index.
    size_t name_or_index; // name of the field (for objects/modules) or index of array
    size_t to_node;

    // Book-keeping fields (not used for serialization)
};

// Nodes
// "node_fields":
//   [ "type", "name", "id", "self_size", "edge_count", "trace_node_id", "detachedness" ]

const int k_node_number_of_fields = 7;
struct Node {
    size_t type; // TODO: point at actual type here?
    string name;
    size_t id; // (vilterp) the memory address, right?
    size_t self_size;
    size_t trace_node_id;  // This is ALWAYS 0 in Javascript heap-snapshots.
    // whether the from_node is attached or dettached from the main application state
    // TODO: .... meaning not yet understood.
    // https://github.com/nodejs/from_node/blob/5fd7a72e1c4fbaf37d3723c4c81dce35c149dc84/deps/v8/include/v8-profiler.h#L739-L745
    int detachedness;  // 0 - unknown,  1 - attached;  2 - detached

    // Book-keeping fields (not used for serialization)
    vector<Edge> edges; // For asserting that we built the edges in the right order
};


struct StringTable {
    typedef unordered_map<string, size_t> MapType;

    MapType map;
    vector<string> strings;

    StringTable() {}
    StringTable(std::initializer_list<string> strs) : strings(strs) {
        for (const auto& str : strs) {
            map.insert({str, map.size()});
        }
    }

    size_t find_or_create_string_id(string key) {
        auto val = map.find(key);
        if (val == map.end()) {
            val = map.insert(val, {key, map.size()});
            strings.push_back(key);
        }
        return val->second;
    }

    void print_json_array(JL_STREAM *stream, bool newlines) {
        jl_printf(stream, "[");
        bool first = true;
        for (const auto &str : strings) {
            if (first) {
                first = false;
            } else {
                jl_printf(stream, ",");
                if (newlines) {
                    jl_printf(stream, "\n");
                }
            }
            // Escape strings for JSON
            // TODO
            print_str_escape_json(stream, str);
        }
        jl_printf(stream, "]");
    }
};

struct HeapSnapshot {
public:

// private:
    vector<Node> nodes;
    // edges are stored on each from_node

    StringTable names;
    StringTable node_types;
    StringTable edge_types;
    unordered_map<void*, size_t> node_ptr_to_index_map;

    size_t num_edges = 0; // For metadata, updated as you add each edge. Needed because edges owned by nodes.
};


// TODO: Do we need to refer to nodes by their index in the from_node array?
//size_t find_or_create_node_id(HeapSnapshot& snapshot, string key) {
//    return find_or_insert_iter(snapshot.nodes_map, key)->second;
//}

HeapSnapshot *g_snapshot = nullptr;

JL_DLLEXPORT int count_nodes = 0;
JL_DLLEXPORT int count_edges = 0;

JL_DLLEXPORT void jl_gc_take_heap_snapshot(JL_STREAM *stream) {
    // Enable snapshotting
    HeapSnapshot snapshot;
    g_snapshot = &snapshot;
    gc_heap_snapshot_enabled = true;

    _add_internal_root(&snapshot);

    // Do GC, which will callback into record_edge_to_gc_snapshot()...
    jl_gc_collect(JL_GC_FULL);

    // Disable snapshotting
    gc_heap_snapshot_enabled = false;
    g_snapshot = nullptr;

    // When we return, the snapshot is full
    // Dump the snapshot
    serialize_heap_snapshot(stream, snapshot);

    // Debugging
    //jl_printf(JL_STDERR, "nodes: %d\n", count_nodes);
    //jl_printf(JL_STDERR, "edges: %d\n", count_edges);
}

// adds a node at id 0 which is the "uber root":
// a synthetic node which points to all the GC roots.
void _add_internal_root(HeapSnapshot *snapshot) {
    Node internal_root{
        snapshot->node_types.find_or_create_string_id("synthetic"),
        "(internal root)", // name
        0, // id
        1, // size

        0, // size_t trace_node_id (unused)
        0, // int detachedness;  // 0 - unknown,  1 - attached;  2 - detached

        // outgoing edges
        vector<Edge>(),
    };
    snapshot->nodes.push_back(internal_root);
}

// mimicking https://github.com/nodejs/node/blob/5fd7a72e1c4fbaf37d3723c4c81dce35c149dc84/deps/v8/src/profiler/heap-snapshot-generator.cc#L597-L597
void record_node_to_gc_snapshot(jl_value_t *a) JL_NOTSAFEPOINT {
    auto val = g_snapshot->node_ptr_to_index_map.find((void*)a);
    if (val != g_snapshot->node_ptr_to_index_map.end()) {
        return;
        //return &g_snapshot->nodes[val->second];
    }

    // Insert a new Node
    size_t self_size = 0;
    string name = "<missing>";
    string node_type = "object";

    if (a == (jl_value_t*)jl_malloc_tag) {
        name = "<malloc>";
    } else {
        jl_datatype_t* type = (jl_datatype_t*)jl_typeof(a);

        if ((uintptr_t)type < 4096U) {
            name = "<unkown>";
        } else if (type == (jl_datatype_t*)jl_buff_tag) {
            name = "<buffer>";
        } else if (type == (jl_datatype_t*)jl_malloc_tag) {
            name = "<malloc>";
        } else if (jl_is_string(a)) {
            node_type = "string";
            name = jl_string_data(a);
            self_size = jl_string_len(a);
        } else if (jl_is_symbol(a)) {
            node_type = "symbol";
            name = jl_symbol_name((jl_sym_t*)a);
            self_size = name.length();
        } else if (jl_is_datatype(type)) {
            self_size = (size_t)jl_datatype_size(type);
            // TODO: get the entire type, including type parameters
            // - option 1: jl_static_show it here:
            //   crashes because it might do GC, and we are already inside GC.
            //
            // - option 2: put type jl_datatype_t* directly in the Node struct:
            //   bad because the jl_datatype_t might be freed before we serialize
            //   the snapshot. Maybe we should allocate some gc-rooted object
            //   and attach all the jl_datatype_t*'s we need to it?
            name = jl_symbol_name(type->name->name);
        }
    }

    g_snapshot->node_ptr_to_index_map.insert(val, {a, g_snapshot->nodes.size()});
    count_nodes += 1;

    Node from_node{
        // We pick a default type here, which will be set for the _targets_ of edges.
        // TODO:  What's a good default?
        g_snapshot->node_types.find_or_create_string_id(node_type), // size_t type;
        name, // string name;
        (size_t)a, // size_t id;
        // We add 1 to self-size for the type tag that all heap-allocated objects have.
        // Also because the Chrome Snapshot viewer ignores size-0 leaves!
        self_size + 1, // size_t self_size;

        0, // size_t trace_node_id (unused)
        0, // int detachedness;  // 0 - unknown,  1 - attached;  2 - detached

        // outgoing edges
        vector<Edge>(),
    };
    g_snapshot->nodes.push_back(from_node);
}

void _gc_heap_snapshot_record_root(jl_value_t *root) {
    record_node_to_gc_snapshot(root);

    // TODO: just make record_node_to_gc_snapshot return this
    auto to_node_idx = g_snapshot->node_ptr_to_index_map[root];

    auto &internal_root = g_snapshot->nodes.front();
    auto edge_type = g_snapshot->edge_types.find_or_create_string_id("internal");
    auto edge_label = g_snapshot->names.find_or_create_string_id("internal_root to root");

    internal_root.edges.push_back(Edge{
        edge_type,
        edge_label,
        to_node_idx,
    });

    g_snapshot->num_edges++;
}

void _gc_heap_snapshot_record_array_edge(jl_value_t *from, jl_value_t *to, size_t index) JL_NOTSAFEPOINT {
    if (!g_snapshot) {
        return;
    }
    _record_gc_edge("array", "element", from, to, index);
}
void _gc_heap_snapshot_record_module_edge(jl_module_t *from, jl_value_t *to, char *name) JL_NOTSAFEPOINT {
    //jl_printf(JL_STDERR, "module: %p  binding:%p  name:%s\n", from, to, name);
    _record_gc_edge("object", "property", (jl_value_t *)from, to,
                    g_snapshot->names.find_or_create_string_id(name));
}
void _gc_heap_snapshot_record_object_edge(jl_value_t *from, jl_value_t *to, size_t field_index) JL_NOTSAFEPOINT {
    jl_datatype_t *type = (jl_datatype_t*)jl_typeof(from);
    // TODO: It seems like NamedTuples should have field names? Maybe there's another way to get them?
    if (jl_is_tuple_type(type) || jl_is_namedtuple_type(type)) {
        // TODO: Maybe not okay to match element and object
        _record_gc_edge("object", "element", from, to, field_index);
        return;
    }
    if (field_index < 0 || jl_datatype_nfields(type) <= field_index) {
        // TODO: We're getting -1 in some cases
        //jl_printf(JL_STDERR, "WARNING - incorrect field index (%zu) for type\n", field_index);
        //jl_(type);
        _record_gc_edge("object", "element", from, to, field_index);
        return;
    }
    jl_svec_t *field_names = jl_field_names(type);
    jl_sym_t *name = (jl_sym_t*)jl_svecref(field_names, field_index);
    const char *field_name = jl_symbol_name(name);

    _record_gc_edge("object", "property", from, to,
                    g_snapshot->names.find_or_create_string_id(field_name));
}
void _gc_heap_snapshot_record_internal_edge(jl_value_t *from, jl_value_t *to) JL_NOTSAFEPOINT {
    // TODO: probably need to inline this here and make some changes
    _record_gc_edge("object", "internal", from, to,
                    g_snapshot->names.find_or_create_string_id("<internal>"));
}
void _gc_heap_snapshot_record_hidden_edge(jl_value_t *from, size_t bytes) JL_NOTSAFEPOINT {
    // TODO: probably need to inline this here and make some changes
    _record_gc_edge("native", "hidden", from, (jl_value_t *)jl_malloc_tag,
                    g_snapshot->names.find_or_create_string_id("<native>"));

    // Add the size to the "unknown malloc" tag
    g_snapshot->nodes[g_snapshot->node_ptr_to_index_map[(jl_value_t*)jl_malloc_tag]].self_size += bytes;
}

static inline void _record_gc_edge(const char *node_type, const char *edge_type,
                                   jl_value_t *a, jl_value_t *b, size_t name_or_index) JL_NOTSAFEPOINT
{
    record_node_to_gc_snapshot(a);
    record_node_to_gc_snapshot(b);

    // Have to look this up because it might not be created for this edge
    auto from_node_idx = g_snapshot->node_ptr_to_index_map[a];

    auto &from_node = g_snapshot->nodes[from_node_idx];
    // TODO: can these ever disagree?:
    from_node.type = g_snapshot->node_types.find_or_create_string_id(node_type);

    from_node.edges.push_back(Edge{
        g_snapshot->edge_types.find_or_create_string_id(edge_type),
        name_or_index,
        g_snapshot->node_ptr_to_index_map[b], // to
    });

    g_snapshot->num_edges += 1;
    count_edges += 1;  // debugging
}

void serialize_heap_snapshot(JL_STREAM *stream, HeapSnapshot &snapshot) {
    // mimicking https://github.com/nodejs/from_node/blob/5fd7a72e1c4fbaf37d3723c4c81dce35c149dc84/deps/v8/src/profiler/heap-snapshot-generator.cc#L2567-L2567
    jl_printf(stream, "{\"snapshot\":{");
    jl_printf(stream, "\"meta\":{");
    jl_printf(stream, "\"node_fields\":[\"type\",\"name\",\"id\",\"self_size\",\"edge_count\",\"trace_node_id\",\"detachedness\"],");
    jl_printf(stream, "\"node_types\":[");
    snapshot.node_types.print_json_array(stream, false);
    jl_printf(stream, ",");
    jl_printf(stream, "\"string\", \"number\", \"number\", \"number\", \"number\", \"number\"],");
    jl_printf(stream, "\"edge_fields\":[\"type\",\"name_or_index\",\"to_node\"],");
    jl_printf(stream, "\"edge_types\":[");
    snapshot.edge_types.print_json_array(stream, false);
    jl_printf(stream, ",");
    jl_printf(stream, "\"string_or_number\",\"from_node\"]");
    jl_printf(stream, "},\n"); // end "meta"
    jl_printf(stream, "\"node_count\":%zu,", snapshot.nodes.size());
    jl_printf(stream, "\"edge_count\":%zu", snapshot.num_edges);
    jl_printf(stream, "},\n"); // end "snapshot"

    jl_printf(stream, "\"nodes\":[");
    bool first_node = true;
    for (const auto &from_node : snapshot.nodes) {
        if (first_node) {
            first_node = false;
        } else {
            jl_printf(stream, ",");
        }
        // ["type","name","id","self_size","edge_count","trace_node_id","detachedness"]
        jl_printf(stream, "%zu", from_node.type);
        jl_printf(stream, ",%zu", snapshot.names.find_or_create_string_id(from_node.name));
        jl_printf(stream, ",%zu", from_node.id);
        jl_printf(stream, ",%zu", from_node.self_size);
        jl_printf(stream, ",%zu", from_node.edges.size());
        jl_printf(stream, ",%zu", from_node.trace_node_id);
        jl_printf(stream, ",%d", from_node.detachedness);
        jl_printf(stream, "\n");
    }
    jl_printf(stream, "],\n");

    jl_printf(stream, "\"edges\":[");
    bool first_edge = true;
    for (const auto &from_node : snapshot.nodes) {
        for (const auto &edge : from_node.edges) {
            if (first_edge) {
                first_edge = false;
            } else {
                jl_printf(stream, ",");
            }
            jl_printf(stream, "%zu", edge.type);
            jl_printf(stream, ",%zu", edge.name_or_index);
            jl_printf(stream, ",%zu", edge.to_node * k_node_number_of_fields);
            jl_printf(stream, "\n");
        }
    }
    jl_printf(stream, "],\n"); // end "edges"

    jl_printf(stream, "\"strings\":");

    snapshot.names.print_json_array(stream, true);

    jl_printf(stream, "}");
}