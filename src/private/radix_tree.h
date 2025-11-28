#ifndef _RADIX_TREE_H_
#define _RADIX_TREE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esphttpd.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Configuration (with fallbacks for non-Kconfig builds)
// ============================================================================

#ifndef CONFIG_HTTPD_MAX_ROUTE_PARAMS
#define CONFIG_HTTPD_MAX_ROUTE_PARAMS 8
#endif

#ifndef CONFIG_HTTPD_MAX_MIDDLEWARE_PER_ROUTER
#define CONFIG_HTTPD_MAX_MIDDLEWARE_PER_ROUTER 8
#endif

#ifndef CONFIG_HTTPD_MAX_ROUTE_MIDDLEWARE
#define CONFIG_HTTPD_MAX_ROUTE_MIDDLEWARE 4
#endif

#ifndef CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE
#define CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE 16
#endif

// ============================================================================
// Forward Declarations
// ============================================================================

typedef struct radix_node radix_node_t;
typedef struct radix_tree radix_tree_t;

// Router structure definition
// Note: httpd_router_t is defined in esphttpd.h as "struct httpd_router*" (pointer type)
struct httpd_router {
    radix_tree_t* tree;
    httpd_middleware_t middlewares[CONFIG_HTTPD_MAX_MIDDLEWARE_PER_ROUTER];
    uint8_t middleware_count;
    httpd_err_handler_t error_handler;
};

// ============================================================================
// Node Types
// ============================================================================

typedef enum {
    NODE_STATIC,      // Exact match: "users", "api"
    NODE_PARAM,       // Parameter: ":id", ":name"
    NODE_WILDCARD,    // Catch-all: "*" (matches rest of path)
} radix_node_type_t;

// ============================================================================
// Handler Storage
// ============================================================================

/**
 * @brief Handler chain node (linked list)
 * Supports multiple handlers per route that execute in sequence (Express-style)
 */
typedef struct handler_node {
    httpd_handler_t handler;
    void* user_ctx;
    struct handler_node* next;
} handler_node_t;

/**
 * @brief Handler storage for a node (one per HTTP method + WebSocket)
 */
typedef struct {
    handler_node_t* http_chains[8];       // Handler chain heads indexed by http_method_t
    httpd_ws_handler_t ws_handler;        // WebSocket handler
    void* ws_user_ctx;
    uint32_t ws_ping_interval;
    uint8_t http_method_mask;             // Bitmask of methods with handlers
    bool has_ws;                          // Has WebSocket handler
    bool has_trailing_slash;              // Route registered with trailing slash (for strict mode)
} node_handlers_t;

// ============================================================================
// Radix Tree Node
// ============================================================================

/**
 * @brief Radix tree node structure
 */
struct radix_node {
    // Segment (edge label)
    char* segment;                        // Heap allocated, e.g. "users", ":id"
    uint8_t segment_len;

    // Node type
    radix_node_type_t type;

    // For NODE_PARAM: parameter name (points into segment after ':')
    const char* param_name;
    uint8_t param_name_len;
    bool is_optional;                     // For :param? optional parameters

    // Static children (sorted by first character for faster lookup)
    radix_node_t** children;              // Dynamic array of child pointers
    uint8_t child_count;
    uint8_t child_capacity;

    // Special children (only one of each allowed per node)
    radix_node_t* param_child;            // :param child
    radix_node_t* wildcard_child;         // * child

    // Handlers at this node (NULL if no route terminates here)
    node_handlers_t* handlers;

    // Per-route middleware
    httpd_middleware_t* middlewares;
    uint8_t middleware_count;
};

// ============================================================================
// Radix Tree
// ============================================================================

/**
 * @brief Radix tree structure
 */
struct radix_tree {
    radix_node_t* root;
    uint16_t node_count;
    uint16_t route_count;
    bool case_sensitive;              // True = paths are case-sensitive (default)
    bool strict;                      // True = trailing slash distinguishes routes
};

// ============================================================================
// Match Result
// ============================================================================

/**
 * @brief Route parameter extracted during lookup
 */
typedef struct {
    const char* key;                 // Parameter name (e.g., "id")
    const char* value;               // Parameter value (e.g., "123")
    uint8_t key_len;
    uint8_t value_len;
} radix_param_t;

/**
 * @brief Result of radix tree lookup
 */
typedef struct {
    bool matched;                    // Route was matched

    // HTTP handler chain (linked list, NOT owned by match - do not free)
    handler_node_t* handler_chain;   // Head of handler chain

    // For backwards compatibility, first handler and context
    httpd_handler_t handler;         // First HTTP handler (or NULL)
    void* user_ctx;                  // First user context

    httpd_ws_handler_t ws_handler;   // WebSocket handler
    void* ws_user_ctx;               // WebSocket user context
    bool is_websocket;               // This is a WebSocket route

    // Collected middleware chain (heap allocated, caller must free)
    httpd_middleware_t* middlewares;
    uint8_t middleware_count;

    // Extracted parameters
    radix_param_t params[CONFIG_HTTPD_MAX_ROUTE_PARAMS];
    uint8_t param_count;
} radix_match_t;

// ============================================================================
// Core Operations
// ============================================================================

/**
 * @brief Create a radix tree
 * @return Pointer to new tree, or NULL on failure
 */
radix_tree_t* radix_tree_create(void);

/**
 * @brief Destroy a radix tree and free all resources
 * @param tree Tree to destroy
 */
void radix_tree_destroy(radix_tree_t* tree);

/**
 * @brief Set case-sensitive routing mode
 * @param tree Tree to configure
 * @param case_sensitive True for case-sensitive (default), false for case-insensitive
 */
void radix_tree_set_case_sensitive(radix_tree_t* tree, bool case_sensitive);

/**
 * @brief Set strict routing mode (trailing slash significance)
 * @param tree Tree to configure
 * @param strict True to distinguish /path from /path/, false to treat as same (default)
 */
void radix_tree_set_strict(radix_tree_t* tree, bool strict);

/**
 * @brief Insert a route into the radix tree
 * @param tree Tree to insert into
 * @param pattern URL pattern (e.g., "/api/users/:id")
 * @param method HTTP method
 * @param handler Request handler
 * @param user_ctx User context passed to handler
 * @param middlewares Array of middleware (can be NULL)
 * @param middleware_count Number of middleware
 * @return HTTPD_OK on success, error code otherwise
 */
httpd_err_t radix_insert(radix_tree_t* tree, const char* pattern,
                         http_method_t method, httpd_handler_t handler,
                         void* user_ctx, httpd_middleware_t* middlewares,
                         uint8_t middleware_count);

/**
 * @brief Insert a WebSocket route into the radix tree
 * @param tree Tree to insert into
 * @param pattern URL pattern (e.g., "/ws")
 * @param handler WebSocket handler
 * @param user_ctx User context passed to handler
 * @param ping_interval_ms Auto-ping interval (0 = disabled)
 * @param middlewares Array of middleware (can be NULL)
 * @param middleware_count Number of middleware
 * @return HTTPD_OK on success, error code otherwise
 */
httpd_err_t radix_insert_ws(radix_tree_t* tree, const char* pattern,
                            httpd_ws_handler_t handler, void* user_ctx,
                            uint32_t ping_interval_ms,
                            httpd_middleware_t* middlewares,
                            uint8_t middleware_count);

/**
 * @brief Look up a route in the radix tree
 * @param tree Tree to search
 * @param path URL path (without query string)
 * @param method HTTP method
 * @param is_websocket Whether this is a WebSocket upgrade
 * @return Match result (caller must free match.middlewares if matched)
 */
radix_match_t radix_lookup(radix_tree_t* tree, const char* path,
                           http_method_t method, bool is_websocket);

// ============================================================================
// Helper Functions (internal)
// ============================================================================

/**
 * @brief Create a new radix node
 * @param segment Segment string (will be copied)
 * @param segment_len Length of segment
 * @param type Node type
 * @return Pointer to new node, or NULL on failure
 */
radix_node_t* radix_node_create(const char* segment, size_t segment_len,
                                radix_node_type_t type);

/**
 * @brief Destroy a radix node and all children
 * @param node Node to destroy
 */
void radix_node_destroy(radix_node_t* node);

/**
 * @brief Find a static child by segment
 * @param node Parent node
 * @param segment Segment to search for
 * @param segment_len Length of segment
 * @return Pointer to child node, or NULL if not found
 */
radix_node_t* radix_find_static_child(radix_node_t* node, const char* segment,
                                      size_t segment_len);

/**
 * @brief Insert a static child in sorted order
 * @param node Parent node
 * @param child Child to insert
 * @return HTTPD_OK on success, error code otherwise
 */
httpd_err_t radix_insert_static_child(radix_node_t* node, radix_node_t* child);

#ifdef __cplusplus
}
#endif

#endif // _RADIX_TREE_H_
