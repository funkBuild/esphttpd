#include "private/radix_tree.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // For strncasecmp
#include "esp_log.h"

static const char* TAG = "RADIX_TREE";

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

    // For NODE_PARAM, extract parameter name (skip ':') and check for optional '?'
    if (type == NODE_PARAM && segment_len > 1 && segment[0] == ':') {
        node->param_name = node->segment + 1;
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

    // Unified binary search - single loop reduces I-cache pressure
    int left = 0;
    int right = node->child_count - 1;

    while (left <= right) {
        int mid = (left + right) >> 1;  // Bit shift faster than division
        radix_node_t* child = node->children[mid];

        // Cache segment_len to avoid multiple dereferences
        size_t child_seg_len = child->segment_len;
        size_t min_len = child_seg_len < segment_len ? child_seg_len : segment_len;

        // Branch hint: case_sensitive is true ~95% of the time
        int cmp = __builtin_expect(case_sensitive, 1) ?
                  memcmp(child->segment, segment, min_len) :
                  strncasecmp(child->segment, segment, min_len);

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
        uint8_t new_capacity = node->child_capacity == 0 ? 4 : node->child_capacity * 2;
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

    ESP_LOGI(TAG, "Inserting route: pattern='%s', method=%d", pattern, method);

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
            child = radix_find_static_child(node, seg_start, seg_len);
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

    node->handlers->http_method_mask |= (1 << method);
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
        } else {
            // Merge with existing middleware
            uint8_t new_count = node->middleware_count + middleware_count;
            if (new_count > CONFIG_HTTPD_MAX_ROUTE_MIDDLEWARE) {
                new_count = CONFIG_HTTPD_MAX_ROUTE_MIDDLEWARE;
            }
            httpd_middleware_t* new_mw = (httpd_middleware_t*)realloc(
                node->middlewares, new_count * sizeof(httpd_middleware_t));
            if (new_mw) {
                node->middlewares = new_mw;
            }
        }

        if (node->middlewares) {
            memcpy(&node->middlewares[node->middleware_count], middlewares,
                   middleware_count * sizeof(httpd_middleware_t));
            node->middleware_count += middleware_count;
        }
    }

    tree->route_count++;
    ESP_LOGI(TAG, "Route inserted successfully (total routes=%d, nodes=%d)",
             tree->route_count, tree->node_count);
    return HTTPD_OK;
}

httpd_err_t radix_insert_ws(radix_tree_t* tree, const char* pattern,
                            httpd_ws_handler_t handler, void* user_ctx,
                            uint32_t ping_interval_ms,
                            httpd_middleware_t* middlewares,
                            uint8_t middleware_count) {
    if (!tree || !pattern || !handler) return HTTPD_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Inserting WebSocket route: pattern='%s'", pattern);

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
            child = radix_find_static_child(node, seg_start, seg_len);
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

    // Copy middleware if provided
    if (middlewares && middleware_count > 0) {
        if (middleware_count > CONFIG_HTTPD_MAX_ROUTE_MIDDLEWARE) {
            middleware_count = CONFIG_HTTPD_MAX_ROUTE_MIDDLEWARE;
        }

        if (!node->middlewares) {
            node->middlewares = (httpd_middleware_t*)malloc(
                middleware_count * sizeof(httpd_middleware_t));
        }

        if (node->middlewares) {
            memcpy(node->middlewares, middlewares,
                   middleware_count * sizeof(httpd_middleware_t));
            node->middleware_count = middleware_count;
        }
    }

    tree->route_count++;
    ESP_LOGI(TAG, "WebSocket route inserted successfully");
    return HTTPD_OK;
}

// ============================================================================
// Route Lookup
// ============================================================================

radix_match_t radix_lookup(radix_tree_t* tree, const char* path,
                           http_method_t method, bool is_websocket) {
    radix_match_t result = {0};

    if (__builtin_expect(!tree || !path, 0)) return result;

    ESP_LOGD(TAG, "Looking up: path='%s', method=%d, ws=%d", path, method, is_websocket);

    // Calculate path end once for efficient wildcard value_len computation
    size_t path_len = strlen(path);
    const char* path_end = path + path_len;
    bool path_has_trailing_slash = false;

    radix_node_t* node = tree->root;
    const char* p = path;
    bool traversed = false;  // Track if we've moved from root

    // Temporary storage for middleware chain building
    httpd_middleware_t mw_stack[CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE];
    uint8_t mw_count = 0;

    while (*p && node) {
        // Skip leading slash
        if (*p == '/') {
            p++;
            // Don't break on trailing slash yet - check for wildcards first
        }

        // Find segment end
        const char* seg_end = p;
        while (*seg_end && *seg_end != '/') seg_end++;
        size_t seg_len = seg_end - p;

        if (seg_len == 0) {
            // Empty segment (trailing slash or double slash)
            // Only check wildcards if we've already traversed past root
            // This prevents "/" from matching the catch-all "*" pattern
            if (traversed && node->wildcard_child) {
                // Wildcard matches empty string
                if (result.param_count < CONFIG_HTTPD_MAX_ROUTE_PARAMS) {
                    result.params[result.param_count].key = "*";
                    result.params[result.param_count].key_len = 1;
                    result.params[result.param_count].value = "";
                    result.params[result.param_count].value_len = 0;
                    result.param_count++;
                }
                node = node->wildcard_child;
                // Collect middleware from wildcard node using memcpy
                if (node->middleware_count > 0) {
                    uint8_t avail = CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE - mw_count;
                    uint8_t to_copy = node->middleware_count < avail ? node->middleware_count : avail;
                    memcpy(&mw_stack[mw_count], node->middlewares, to_copy * sizeof(httpd_middleware_t));
                    mw_count += to_copy;
                }
            }
            break; // End of path
        }

        traversed = true;  // We're about to traverse to a child

        radix_node_t* next = NULL;

        // 1. Try static children first (respecting case_sensitive setting)
        next = radix_find_static_child_internal(node, p, seg_len, tree->case_sensitive);
        if (__builtin_expect(next != NULL, 1)) {  // Static match is most common
            p = seg_end;
            node = next;
            goto collect_middleware;
        }

        // Check if there are more path segments after this one
        bool has_more_segments = (*seg_end != '\0');

        // 2. Try wildcard first if there are multiple remaining segments
        //    (wildcards capture the rest of the path, so they're preferred for multi-segment)
        if (__builtin_expect(node->wildcard_child && has_more_segments, 0)) {
            // Wildcard captures rest of path
            if (result.param_count < CONFIG_HTTPD_MAX_ROUTE_PARAMS) {
                result.params[result.param_count].key = "*";
                result.params[result.param_count].key_len = 1;
                result.params[result.param_count].value = p;
                result.params[result.param_count].value_len = path_end - p;  // Use pointer arithmetic instead of strlen
                result.param_count++;
            }
            node = node->wildcard_child;
            p = ""; // Consumed entire path
            goto collect_middleware;
        }

        // 3. Try parameter child for single segment
        if (__builtin_expect(node->param_child != NULL, 1)) {
            if (result.param_count < CONFIG_HTTPD_MAX_ROUTE_PARAMS) {
                result.params[result.param_count].key = node->param_child->param_name;
                result.params[result.param_count].key_len = node->param_child->param_name_len;
                result.params[result.param_count].value = p;
                result.params[result.param_count].value_len = seg_len;
                result.param_count++;
            }
            p = seg_end;
            node = node->param_child;
            goto collect_middleware;
        }

        // 4. Try wildcard for last segment (single segment case)
        if (__builtin_expect(node->wildcard_child != NULL, 0)) {
            if (result.param_count < CONFIG_HTTPD_MAX_ROUTE_PARAMS) {
                result.params[result.param_count].key = "*";
                result.params[result.param_count].key_len = 1;
                result.params[result.param_count].value = p;
                result.params[result.param_count].value_len = seg_len;
                result.param_count++;
            }
            node = node->wildcard_child;
            p = seg_end;
            goto collect_middleware;
        }

        // No match found
        ESP_LOGD(TAG, "No match for segment '%.*s'", (int)seg_len, p);
        return result;

    collect_middleware:
        // Collect middleware from this node using memcpy
        if (node->middleware_count > 0) {
            uint8_t avail = CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE - mw_count;
            uint8_t to_copy = node->middleware_count < avail ? node->middleware_count : avail;
            memcpy(&mw_stack[mw_count], node->middlewares, to_copy * sizeof(httpd_middleware_t));
            mw_count += to_copy;
        }
    }

    // Check if we should traverse to optional param child (for routes like /users/:id?)
    // This handles the case where path is "/users" but route is "/users/:id?"
    if (node && !node->handlers && node->param_child &&
        node->param_child->is_optional && node->param_child->handlers) {
        node = node->param_child;
        // Note: param is not added to result.params since it wasn't provided
        // Collect middleware from this node using memcpy
        if (node->middleware_count > 0) {
            uint8_t avail = CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE - mw_count;
            uint8_t to_copy = node->middleware_count < avail ? node->middleware_count : avail;
            memcpy(&mw_stack[mw_count], node->middlewares, to_copy * sizeof(httpd_middleware_t));
            mw_count += to_copy;
        }
    }

    // Check for handler at terminal node
    if (node && node->handlers) {
        // In strict mode, trailing slashes must match
        bool strict_ok = true;
        if (tree->strict) {
            // Use pre-calculated path_len (already computed at function start)
            path_has_trailing_slash = (path_len > 1 && path[path_len - 1] == '/');
            strict_ok = (path_has_trailing_slash == node->handlers->has_trailing_slash);
        }

        if (!strict_ok) {
            ESP_LOGD(TAG, "Strict mode: trailing slash mismatch (path=%d, route=%d)",
                     path_has_trailing_slash, node->handlers->has_trailing_slash);
        } else if (is_websocket && node->handlers->has_ws) {
            result.matched = true;
            result.ws_handler = node->handlers->ws_handler;
            result.ws_user_ctx = node->handlers->ws_user_ctx;
            result.is_websocket = true;
            ESP_LOGD(TAG, "Matched WebSocket route");
        } else if (!is_websocket) {
            // Direct chain check - non-null chain implies method is supported
            handler_node_t* chain = node->handlers->http_chains[method];
            if (chain) {
                result.matched = true;
                result.handler_chain = chain;
                // For backwards compatibility, set first handler
                result.handler = chain->handler;
                result.user_ctx = chain->user_ctx;
                result.is_websocket = false;
                ESP_LOGD(TAG, "Matched HTTP route (method=%d)", method);
            }
        }

        // Only collect terminal middleware if we matched (using memcpy)
        if (result.matched && node->middleware_count > 0) {
            uint8_t avail = CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE - mw_count;
            uint8_t to_copy = node->middleware_count < avail ? node->middleware_count : avail;
            memcpy(&mw_stack[mw_count], node->middlewares, to_copy * sizeof(httpd_middleware_t));
            mw_count += to_copy;
        }

        // Allocate and copy middleware if matched
        if (result.matched && mw_count > 0) {
            result.middlewares = (httpd_middleware_t*)malloc(
                mw_count * sizeof(httpd_middleware_t));
            if (result.middlewares) {
                memcpy(result.middlewares, mw_stack,
                       mw_count * sizeof(httpd_middleware_t));
                result.middleware_count = mw_count;
            }
        }
    }

    return result;
}
