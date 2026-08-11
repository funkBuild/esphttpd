#include "private/radix_tree.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // For strncasecmp
#include "esp_log.h"

static const char TAG[] = "RADIX_TREE";

// ============================================================================
// Node Creation and Destruction
// ============================================================================

radix_node_t* radix_node_create(const char* segment, size_t segment_len,
                                radix_node_type_t type) {
    radix_node_t* node = (radix_node_t*)calloc(1, sizeof(radix_node_t));
    if (!node) {
        ESP_LOGE(TAG, "Failed to allocate node");
        return NULL;
    }

    // Copy segment
    node->segment = (char*)malloc(segment_len + 1);
    if (!node->segment) {
        ESP_LOGE(TAG, "Failed to allocate segment");
        free(node);
        return NULL;
    }
    memcpy(node->segment, segment, segment_len);
    node->segment[segment_len] = '\0';
    node->segment_len = segment_len;
    node->type = type;

    // For NODE_PARAM, extract parameter name length (skip ':') and check for
    // optional '?'. The name itself is not stored: it is always segment[1..]
    // and is derived at the lookup read site from segment/param_name_len.
    if (type == NODE_PARAM && segment_len > 1 && segment[0] == ':') {
        node->param_name_len = segment_len - 1;

        // Check for optional parameter (:param?)
        if (node->param_name_len > 0 && node->segment[segment_len - 1] == '?') {
            node->is_optional = true;
            node->param_name_len--;  // Exclude '?' from param name
            // Also update the segment to exclude '?' for cleaner matching
            node->segment[segment_len - 1] = '\0';
            node->segment_len--;
        }
    }

    ESP_LOGD(TAG, "Created node: segment='%s', type=%d", node->segment, type);
    return node;
}

void radix_node_destroy(radix_node_t* node) {
    if (!node) return;

    ESP_LOGD(TAG, "Destroying node: segment='%s'", node->segment);

    // Destroy static children
    for (uint8_t i = 0; i < node->child_count; i++) {
        radix_node_destroy(node->children[i]);
    }
    free(node->children);

    // Destroy special children
    radix_node_destroy(node->param_child);
    radix_node_destroy(node->wildcard_child);

    // Free handler chains
    if (node->handlers) {
        for (int i = 0; i < 8; i++) {
            handler_node_t* h = node->handlers->http_chains[i];
            while (h) {
                handler_node_t* next = h->next;
                free(h);
                h = next;
            }
        }
        free(node->handlers);
    }

    // Free node data
    free(node->segment);
    free(node->middlewares);
    free(node);
}

// ============================================================================
// Tree Creation and Destruction
// ============================================================================

radix_tree_t* radix_tree_create(void) {
    radix_tree_t* tree = (radix_tree_t*)calloc(1, sizeof(radix_tree_t));
    if (!tree) {
        ESP_LOGE(TAG, "Failed to allocate tree");
        return NULL;
    }

    // Create root node with empty segment
    tree->root = radix_node_create("", 0, NODE_STATIC);
    if (!tree->root) {
        free(tree);
        return NULL;
    }

    tree->node_count = 1;
    tree->route_count = 0;
    tree->case_sensitive = true;   // Default: case-sensitive matching
    tree->strict = false;          // Default: ignore trailing slashes

    ESP_LOGI(TAG, "Created radix tree");
    return tree;
}

void radix_tree_destroy(radix_tree_t* tree) {
    if (!tree) return;

    ESP_LOGI(TAG, "Destroying radix tree (routes=%d, nodes=%d)",
             tree->route_count, tree->node_count);

    radix_node_destroy(tree->root);
    free(tree);
}

void radix_tree_set_case_sensitive(radix_tree_t* tree, bool case_sensitive) {
    if (tree) {
        tree->case_sensitive = case_sensitive;
        ESP_LOGI(TAG, "Case-sensitive routing: %s", case_sensitive ? "enabled" : "disabled");
    }
}

void radix_tree_set_strict(radix_tree_t* tree, bool strict) {
    if (tree) {
        tree->strict = strict;
        ESP_LOGI(TAG, "Strict routing: %s", strict ? "enabled" : "disabled");
    }
}

// ============================================================================
// Child Management
// ============================================================================

// Internal find with case_sensitive parameter (unified binary search)
static radix_node_t* radix_find_static_child_internal(radix_node_t* node, const char* segment,
                                                       size_t segment_len, bool case_sensitive) {
    if (__builtin_expect(!node || !segment || segment_len == 0, 0)) return NULL;

    if (node->child_count == 0) return NULL;

    // Case-insensitive lookup must NOT binary-search: children are sorted by
    // case-SENSITIVE byte order ('B' < 'a'), so a case-folded comparison over
    // that ordering skips matching entries. Linear scan instead (rare path).
    if (__builtin_expect(!case_sensitive, 0)) {
        for (int i = 0; i < (int)node->child_count; i++) {
            radix_node_t* child = node->children[i];
            if (child->segment_len == segment_len &&
                strncasecmp(child->segment, segment, segment_len) == 0) {
                return child;
            }
        }
        return NULL;
    }

    // Unified binary search - single loop reduces I-cache pressure
    int left = 0;
    int right = (int)node->child_count - 1;

    while (left <= right) {
        int mid = (left + right) >> 1;  // Bit shift faster than division
        radix_node_t* child = node->children[mid];

        // Cache segment_len to avoid multiple dereferences
        size_t child_seg_len = child->segment_len;
        size_t min_len = child_seg_len < segment_len ? child_seg_len : segment_len;

        int cmp = memcmp(child->segment, segment, min_len);

        if (cmp == 0) {
            // Prefixes match, compare by length (use cached child_seg_len)
            if (child_seg_len == segment_len) {
                return child;  // Exact match
            } else if (child_seg_len < segment_len) {
                left = mid + 1;  // Child is shorter, search right
            } else {
                right = mid - 1;  // Child is longer, search left
            }
        } else if (cmp < 0) {
            left = mid + 1;  // Child is lexicographically less, search right
        } else {
            right = mid - 1;  // Child is lexicographically greater, search left
        }
    }

    return NULL;
}

// Public API - case-sensitive by default (for insertion and other uses)
radix_node_t* radix_find_static_child(radix_node_t* node, const char* segment,
                                      size_t segment_len) {
    return radix_find_static_child_internal(node, segment, segment_len, true);
}

httpd_err_t radix_insert_static_child(radix_node_t* node, radix_node_t* child) {
    if (!node || !child) return HTTPD_ERR_INVALID_ARG;

    // Grow children array if needed
    if (node->child_count >= node->child_capacity) {
        // Hard cap: capacity saturates at 255 (uint8_t); inserting past it
        // would write out of bounds and wrap child_count
        if (node->child_count == 255) {
            ESP_LOGE(TAG, "Node child limit (255) reached");
            return HTTPD_ERR_NO_MEM;
        }
        // First allocation is a single slot: the many linear-chain nodes carry
        // exactly one static child, so 0->4 wasted ~12 bytes each. The doubling
        // below (1->2->4->...) handles fan-out nodes.
        uint8_t new_capacity = node->child_capacity == 0 ? 1
            : (node->child_capacity <= 127 ? node->child_capacity * 2 : 255);
        radix_node_t** new_children = (radix_node_t**)realloc(
            node->children, new_capacity * sizeof(radix_node_t*));
        if (!new_children) {
            ESP_LOGE(TAG, "Failed to grow children array");
            return HTTPD_ERR_NO_MEM;
        }
        node->children = new_children;
        node->child_capacity = new_capacity;
    }

    // Find insertion position (keep sorted lexicographically)
    int pos = 0;
    while (pos < node->child_count) {
        radix_node_t* existing = node->children[pos];
        size_t min_len = existing->segment_len < child->segment_len ?
                         existing->segment_len : child->segment_len;
        int cmp = memcmp(existing->segment, child->segment, min_len);

        if (cmp < 0 || (cmp == 0 && existing->segment_len < child->segment_len)) {
            pos++;  // Existing is lexicographically less, continue
        } else {
            break;  // Found insertion point
        }
    }

    // Shift existing children right (memmove is faster than manual loop)
    if (node->child_count > pos) {
        memmove(&node->children[pos + 1], &node->children[pos],
                (node->child_count - pos) * sizeof(radix_node_t*));
    }

    // Insert new child
    node->children[pos] = child;
    node->child_count++;

    ESP_LOGD(TAG, "Inserted static child '%s' at position %d", child->segment, pos);
    return HTTPD_OK;
}

// ============================================================================
// Route Insertion
// ============================================================================

httpd_err_t radix_insert(radix_tree_t* tree, const char* pattern,
                         http_method_t method, httpd_handler_t handler,
                         void* user_ctx, httpd_middleware_t* middlewares,
                         uint8_t middleware_count) {
    if (!tree || !pattern || !handler) return HTTPD_ERR_INVALID_ARG;
    if (method < 0 || method > HTTP_ANY) return HTTPD_ERR_INVALID_ARG;

    ESP_LOGD(TAG, "Inserting route: pattern='%s', method=%d", pattern, method);

    // Check if pattern has trailing slash (for strict mode)
    size_t pattern_len = strlen(pattern);
    bool has_trailing_slash = (pattern_len > 1 && pattern[pattern_len - 1] == '/');

    radix_node_t* node = tree->root;
    const char* p = pattern;

    // Walk the tree, creating nodes as needed
    while (*p) {
        // Skip leading slash
        if (*p == '/') {
            p++;
            if (!*p) break; // Trailing slash
        }

        // Extract next segment
        const char* seg_start = p;
        while (*p && *p != '/') p++;
        size_t seg_len = p - seg_start;

        if (seg_len == 0) break; // Double slash or end

        // Determine segment type
        radix_node_type_t type = NODE_STATIC;
        if (seg_start[0] == ':') {
            type = NODE_PARAM;
        } else if (seg_start[0] == '*') {
            type = NODE_WILDCARD;
        }

        // Find or create child
        radix_node_t* child = NULL;

        if (type == NODE_PARAM) {
            // Parameter child
            if (!node->param_child) {
                node->param_child = radix_node_create(seg_start, seg_len, type);
                if (!node->param_child) return HTTPD_ERR_NO_MEM;
                tree->node_count++;
            }
            child = node->param_child;
        } else if (type == NODE_WILDCARD) {
            // Wildcard child
            if (!node->wildcard_child) {
                node->wildcard_child = radix_node_create(seg_start, seg_len, type);
                if (!node->wildcard_child) return HTTPD_ERR_NO_MEM;
                tree->node_count++;
            }
            child = node->wildcard_child;
        } else {
            // Static child
            child = radix_find_static_child_internal(node, seg_start, seg_len, tree->case_sensitive);
            if (!child) {
                child = radix_node_create(seg_start, seg_len, type);
                if (!child) return HTTPD_ERR_NO_MEM;
                httpd_err_t err = radix_insert_static_child(node, child);
                if (err != HTTPD_OK) {
                    radix_node_destroy(child);
                    return err;
                }
                tree->node_count++;
            }
        }

        node = child;
    }

    // Install handler at terminal node
    if (!node->handlers) {
        node->handlers = (node_handlers_t*)calloc(1, sizeof(node_handlers_t));
        if (!node->handlers) {
            ESP_LOGE(TAG, "Failed to allocate handlers");
            return HTTPD_ERR_NO_MEM;
        }
    }

    // Create new handler node
    handler_node_t* new_handler = (handler_node_t*)malloc(sizeof(handler_node_t));
    if (!new_handler) {
        ESP_LOGE(TAG, "Failed to allocate handler node");
        return HTTPD_ERR_NO_MEM;
    }
    new_handler->handler = handler;
    new_handler->user_ctx = user_ctx;
    new_handler->next = NULL;

    // Append to end of chain for this method
    handler_node_t** chain_ptr = &node->handlers->http_chains[method];
    while (*chain_ptr) {
        chain_ptr = &(*chain_ptr)->next;
    }
    *chain_ptr = new_handler;

    node->handlers->has_trailing_slash = has_trailing_slash;
    ESP_LOGD(TAG, "Added handler to chain (method=%d, trailing_slash=%d)", method, has_trailing_slash);

    // Copy middleware if provided
    if (middlewares && middleware_count > 0) {
        if (middleware_count > CONFIG_HTTPD_MAX_ROUTE_MIDDLEWARE) {
            ESP_LOGW(TAG, "Truncating middleware count from %d to %d",
                     middleware_count, CONFIG_HTTPD_MAX_ROUTE_MIDDLEWARE);
            middleware_count = CONFIG_HTTPD_MAX_ROUTE_MIDDLEWARE;
        }

        // Allocate or reallocate middleware array
        if (!node->middlewares) {
            node->middlewares = (httpd_middleware_t*)malloc(
                middleware_count * sizeof(httpd_middleware_t));
            if (!node->middlewares) {
                ESP_LOGE(TAG, "Failed to allocate middleware array");
                return HTTPD_ERR_NO_MEM;
            }
        } else {
            // Merge with existing middleware
            uint8_t new_count = node->middleware_count + middleware_count;
            if (new_count > CONFIG_HTTPD_MAX_ROUTE_MIDDLEWARE) {
                new_count = CONFIG_HTTPD_MAX_ROUTE_MIDDLEWARE;
            }
            httpd_middleware_t* new_mw = (httpd_middleware_t*)realloc(
                node->middlewares, new_count * sizeof(httpd_middleware_t));
            if (!new_mw) {
                ESP_LOGE(TAG, "Failed to realloc middleware array");
                return HTTPD_ERR_NO_MEM;
            }
            node->middlewares = new_mw;
            // Clamp actual copy count to available space
            middleware_count = new_count - node->middleware_count;
        }

        if (node->middlewares) {
            memcpy(&node->middlewares[node->middleware_count], middlewares,
                   middleware_count * sizeof(httpd_middleware_t));
            node->middleware_count += middleware_count;
        }
    }

    tree->route_count++;
    ESP_LOGD(TAG, "Route inserted successfully (total routes=%d, nodes=%d)",
             tree->route_count, tree->node_count);
    return HTTPD_OK;
}

httpd_err_t radix_insert_ws(radix_tree_t* tree, const char* pattern,
                            httpd_ws_handler_t handler, void* user_ctx,
                            uint32_t ping_interval_ms,
                            httpd_middleware_t* middlewares,
                            uint8_t middleware_count) {
    if (!tree || !pattern || !handler) return HTTPD_ERR_INVALID_ARG;

    ESP_LOGD(TAG, "Inserting WebSocket route: pattern='%s'", pattern);

    radix_node_t* node = tree->root;
    const char* p = pattern;

    // Walk the tree, creating nodes as needed (same as HTTP routes)
    while (*p) {
        if (*p == '/') {
            p++;
            if (!*p) break;
        }

        const char* seg_start = p;
        while (*p && *p != '/') p++;
        size_t seg_len = p - seg_start;

        if (seg_len == 0) break;

        radix_node_type_t type = NODE_STATIC;
        if (seg_start[0] == ':') {
            type = NODE_PARAM;
        } else if (seg_start[0] == '*') {
            type = NODE_WILDCARD;
        }

        radix_node_t* child = NULL;

        if (type == NODE_PARAM) {
            if (!node->param_child) {
                node->param_child = radix_node_create(seg_start, seg_len, type);
                if (!node->param_child) return HTTPD_ERR_NO_MEM;
                tree->node_count++;
            }
            child = node->param_child;
        } else if (type == NODE_WILDCARD) {
            if (!node->wildcard_child) {
                node->wildcard_child = radix_node_create(seg_start, seg_len, type);
                if (!node->wildcard_child) return HTTPD_ERR_NO_MEM;
                tree->node_count++;
            }
            child = node->wildcard_child;
        } else {
            child = radix_find_static_child_internal(node, seg_start, seg_len, tree->case_sensitive);
            if (!child) {
                child = radix_node_create(seg_start, seg_len, type);
                if (!child) return HTTPD_ERR_NO_MEM;
                httpd_err_t err = radix_insert_static_child(node, child);
                if (err != HTTPD_OK) {
                    radix_node_destroy(child);
                    return err;
                }
                tree->node_count++;
            }
        }

        node = child;
    }

    // Install WebSocket handler at terminal node
    if (!node->handlers) {
        node->handlers = (node_handlers_t*)calloc(1, sizeof(node_handlers_t));
        if (!node->handlers) return HTTPD_ERR_NO_MEM;
    }

    node->handlers->ws_handler = handler;
    node->handlers->ws_user_ctx = user_ctx;
    node->handlers->ws_ping_interval = ping_interval_ms;
    node->handlers->has_ws = true;

    // Copy middleware if provided (mirrors radix_insert: must MERGE behind
    // any entries an earlier HTTP-route insert placed on this node — a plain
    // overwrite-memcpy clobbered them and could overflow a smaller allocation)
    if (middlewares && middleware_count > 0) {
        if (middleware_count > CONFIG_HTTPD_MAX_ROUTE_MIDDLEWARE) {
            middleware_count = CONFIG_HTTPD_MAX_ROUTE_MIDDLEWARE;
        }

        if (!node->middlewares) {
            node->middlewares = (httpd_middleware_t*)malloc(
                middleware_count * sizeof(httpd_middleware_t));
            if (!node->middlewares) {
                ESP_LOGE(TAG, "Failed to allocate WS middleware array");
                return HTTPD_ERR_NO_MEM;
            }
            memcpy(node->middlewares, middlewares,
                   middleware_count * sizeof(httpd_middleware_t));
            node->middleware_count = middleware_count;
        } else {
            uint8_t new_count = node->middleware_count + middleware_count;
            if (new_count > CONFIG_HTTPD_MAX_ROUTE_MIDDLEWARE) {
                new_count = CONFIG_HTTPD_MAX_ROUTE_MIDDLEWARE;
            }
            uint8_t to_copy = new_count - node->middleware_count;
            if (to_copy > 0) {
                httpd_middleware_t* new_mw = (httpd_middleware_t*)realloc(
                    node->middlewares, new_count * sizeof(httpd_middleware_t));
                if (!new_mw) {
                    ESP_LOGE(TAG, "Failed to realloc WS middleware array");
                    return HTTPD_ERR_NO_MEM;
                }
                memcpy(&new_mw[node->middleware_count], middlewares,
                       to_copy * sizeof(httpd_middleware_t));
                node->middlewares = new_mw;
                node->middleware_count = new_count;
            }
        }
    }

    tree->route_count++;
    ESP_LOGD(TAG, "WebSocket route inserted successfully");
    return HTTPD_OK;
}

// ============================================================================
// Route Lookup
// ============================================================================

// Append a node's per-route middleware into the caller's destination buffer,
// clamped to CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE. No-op if mw_out is NULL.
static void radix_collect_mw(radix_node_t* node, httpd_middleware_t* mw_out,
                             uint8_t* mw_count) {
    if (mw_out && node->middleware_count > 0) {
        uint8_t avail = (*mw_count < CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE)
                        ? CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE - *mw_count : 0;
        uint8_t to_copy = node->middleware_count < avail ? node->middleware_count : avail;
        memcpy(&mw_out[*mw_count], node->middlewares, to_copy * sizeof(httpd_middleware_t));
        *mw_count += to_copy;
    }
}

// Decide whether a terminal `node` produces a match for this request and, if so,
// populate `result`. Returns true on a match. Honors strict trailing-slash mode,
// WebSocket vs HTTP selection, and the HTTP_ANY fallback chain.
static bool radix_try_terminal(radix_tree_t* tree, radix_node_t* node,
                               http_method_t method, bool is_websocket,
                               bool path_has_trailing_slash,
                               radix_match_t* result) {
    if (!node->handlers) return false;

    // In strict mode, trailing slashes must match
    if (__builtin_expect(tree->strict, 0)) {
        if (path_has_trailing_slash != node->handlers->has_trailing_slash) {
            ESP_LOGD(TAG, "Strict mode: trailing slash mismatch for route");
            return false;
        }
    }

    if (is_websocket) {
        if (node->handlers->has_ws) {
            result->matched = true;
            result->ws_handler = node->handlers->ws_handler;
            result->ws_user_ctx = node->handlers->ws_user_ctx;
            result->is_websocket = true;
            ESP_LOGD(TAG, "Matched WebSocket route");
            return true;
        }
        return false;
    }

    if (method >= 0 && method < 8) {
        // Direct chain check - non-null chain implies method is supported
        handler_node_t* chain = node->handlers->http_chains[method];
        // Fall back to HTTP_ANY: routes registered with it would otherwise be
        // unreachable since no request ever parses to method 7
        if (!chain && method != HTTP_ANY) {
            chain = node->handlers->http_chains[HTTP_ANY];
        }
        if (chain) {
            result->matched = true;
            result->handler_chain = chain;
            // For backwards compatibility, set first handler
            result->handler = chain->handler;
            result->user_ctx = chain->user_ctx;
            result->is_websocket = false;
            ESP_LOGD(TAG, "Matched HTTP route (method=%d)", method);
            return true;
        }
    }
    return false;
}

// Recursive, backtracking segment matcher. Consumes one path segment per call
// and recurses for the remainder. At each node the branches are tried in
// precedence order -- exact static child, then :param child, then * wildcard
// (last resort, even when segments remain). When a chosen branch dead-ends the
// matcher unwinds and tries the next alternative, rolling back any params bound
// and middleware collected on the abandoned branch so they do not leak into the
// eventual result.
//
// `node`'s own middleware is collected by the caller before it recurses in here
// (the top-level root node carries none). Returns true once a terminal match is
// found, with `result` and the middleware buffer finalized.
static bool radix_lookup_rec(radix_tree_t* tree, radix_node_t* node,
                             const char* p, bool traversed,
                             http_method_t method, bool is_websocket,
                             bool path_has_trailing_slash,
                             radix_match_t* result,
                             httpd_middleware_t* mw_out, uint8_t* mw_count) {
    // Snapshot for rolling back branches that are tried and then abandoned.
    uint8_t mw_saved = *mw_count;
    uint8_t param_saved = result->param_count;

    // Skip a single leading slash, then isolate the next segment.
    const char* q = p;
    if (*q == '/') q++;

    const char* seg_end = q;
    while (*seg_end && *seg_end != '/') seg_end++;
    size_t seg_len = (size_t)(seg_end - q);

    if (seg_len == 0) {
        // End of path (or an empty segment from a trailing/double slash).
        // Terminal precedence: this node's own (exact) handler first, then an
        // optional-param child, then an empty-value wildcard as last resort.
        if (radix_try_terminal(tree, node, method, is_websocket,
                               path_has_trailing_slash, result)) {
            return true;
        }

        // Optional param child, e.g. route /users/:id? matched by path /users.
        if (node->param_child && node->param_child->is_optional &&
            node->param_child->handlers) {
            radix_collect_mw(node->param_child, mw_out, mw_count);
            // Param intentionally NOT bound: it was not provided in the path.
            if (radix_try_terminal(tree, node->param_child, method, is_websocket,
                                   path_has_trailing_slash, result)) {
                return true;
            }
            *mw_count = mw_saved;
        }

        // Empty-value wildcard. Only once we have traversed past the root, so
        // that "/" does not spuriously match a catch-all "*".
        if (traversed && node->wildcard_child) {
            if (result->param_count < CONFIG_HTTPD_MAX_ROUTE_PARAMS) {
                result->params[result->param_count].key = "*";
                result->params[result->param_count].key_len = 1;
                result->params[result->param_count].value = "";
                result->params[result->param_count].value_len = 0;
                result->param_count++;
            }
            radix_collect_mw(node->wildcard_child, mw_out, mw_count);
            if (radix_try_terminal(tree, node->wildcard_child, method, is_websocket,
                                   path_has_trailing_slash, result)) {
                return true;
            }
            *mw_count = mw_saved;
            result->param_count = param_saved;
        }

        return false;
    }

    // 1. Exact static child (highest precedence, most common case).
    radix_node_t* schild = radix_find_static_child_internal(node, q, seg_len,
                                                            tree->case_sensitive);
    if (__builtin_expect(schild != NULL, 1)) {
        radix_collect_mw(schild, mw_out, mw_count);
        if (radix_lookup_rec(tree, schild, seg_end, true, method, is_websocket,
                             path_has_trailing_slash, result, mw_out, mw_count)) {
            return true;
        }
        // Abandoned: unwind middleware and any params bound deeper.
        *mw_count = mw_saved;
        result->param_count = param_saved;
    }

    // 2. Parameter child (binds exactly this one segment).
    if (node->param_child) {
        radix_node_t* pc = node->param_child;
        if (result->param_count < CONFIG_HTTPD_MAX_ROUTE_PARAMS) {
            // Param name is segment[1..] (the ':' prefix is stripped at create).
            result->params[result->param_count].key = pc->segment + 1;
            result->params[result->param_count].key_len = pc->param_name_len;
            result->params[result->param_count].value = q;
            result->params[result->param_count].value_len = (uint16_t)seg_len;
            result->param_count++;
        }
        radix_collect_mw(pc, mw_out, mw_count);
        if (radix_lookup_rec(tree, pc, seg_end, true, method, is_websocket,
                             path_has_trailing_slash, result, mw_out, mw_count)) {
            return true;
        }
        // Abandoned: roll back this param binding so it cannot leak into a
        // different route that ends up matching.
        *mw_count = mw_saved;
        result->param_count = param_saved;
    }

    // 3. Wildcard child (last resort; captures the entire remaining path).
    if (node->wildcard_child) {
        if (result->param_count < CONFIG_HTTPD_MAX_ROUTE_PARAMS) {
            result->params[result->param_count].key = "*";
            result->params[result->param_count].key_len = 1;
            result->params[result->param_count].value = q;
            result->params[result->param_count].value_len = (uint16_t)strlen(q);
            result->param_count++;
        }
        radix_collect_mw(node->wildcard_child, mw_out, mw_count);
        if (radix_lookup_rec(tree, node->wildcard_child, "", true, method,
                             is_websocket, path_has_trailing_slash, result,
                             mw_out, mw_count)) {
            return true;
        }
        *mw_count = mw_saved;
        result->param_count = param_saved;
    }

    ESP_LOGD(TAG, "No match for segment '%.*s'", (int)seg_len, q);
    return false;
}

void radix_lookup(radix_tree_t* tree, const char* path,
                  http_method_t method, bool is_websocket,
                  radix_match_t* result,
                  httpd_middleware_t* mw_out, uint8_t* mw_count_out) {
    // Initialize all result fields for early-return safety
    memset(result, 0, sizeof(radix_match_t));

    if (mw_count_out) *mw_count_out = 0;

    if (__builtin_expect(!tree || !path || !tree->root, 0)) return;

    ESP_LOGD(TAG, "Looking up: path='%s', method=%d, ws=%d", path, method, is_websocket);

    // Precompute the request's trailing-slash state once (strict mode only).
    bool path_has_trailing_slash = false;
    if (__builtin_expect(tree->strict, 0)) {
        size_t path_len = strlen(path);
        path_has_trailing_slash = (path_len > 1 && path[path_len - 1] == '/');
    }

    // Middleware is written directly into the caller's destination buffer.
    uint8_t mw_count = 0;
    bool matched = radix_lookup_rec(tree, tree->root, path, false, method,
                                    is_websocket, path_has_trailing_slash,
                                    result, mw_out, &mw_count);

    if (matched && mw_count_out) {
        *mw_count_out = mw_count;
    }
}
