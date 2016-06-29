/*
 * listener.c - contains functions for processing queries.
 *
 * author       : Jeroen van der Heijden
 * email        : jeroen@transceptor.technology
 * copyright    : 2016, Transceptor Technology
 *
 * changes
 *  - initial version, 10-03-2016
 *
 */
#include <siri/parser/listener.h>
#include <siri/parser/walkers.h>
#include <siri/parser/queries.h>
#include <logger/logger.h>
#include <siri/siri.h>
#include <siri/db/query.h>
#include <siri/db/series.h>
#include <siri/db/props.h>
#include <siri/db/shard.h>
#include <siri/net/protocol.h>
#include <siri/db/servers.h>
#include <inttypes.h>
#include <sys/time.h>
#include <qpack/qpack.h>
#include <siri/db/user.h>
#include <siri/db/users.h>
#include <strextra/strextra.h>
#include <assert.h>
#include <math.h>
#include <siri/db/nodes.h>
#include <cexpr/cexpr.h>
#include <siri/net/socket.h>

#define QP_ADD_SUCCESS qp_add_raw(query->packer, "success_msg", 11);
#define DEFAULT_ALLOC_COLUMNS 8
#define IS_MASTER (query->flags & SIRIDB_QUERY_FLAG_MASTER)

static void decref_server_object(uv_handle_t * handle);
static void decref_user_object(uv_handle_t * handle);

static void enter_access_expr(uv_async_t * handle);
static void enter_alter_server(uv_async_t * handle);
static void enter_alter_user(uv_async_t * handle);
static void enter_count_stmt(uv_async_t * handle);
static void enter_create_user_stmt(uv_async_t * handle);
static void enter_drop_stmt(uv_async_t * handle);
static void enter_grant_stmt(uv_async_t * handle);
static void enter_grant_user_stmt(uv_async_t * handle);
static void enter_limit_expr(uv_async_t * handle);
static void enter_list_stmt(uv_async_t * handle);
static void enter_revoke_stmt(uv_async_t * handle);
static void enter_revoke_user_stmt(uv_async_t * handle);
static void enter_select_stmt(uv_async_t * handle);
static void enter_set_password(uv_async_t * handle);
static void enter_series_name(uv_async_t * handle);
static void enter_series_match(uv_async_t * handle);
static void enter_timeit_stmt(uv_async_t * handle);
static void enter_where_xxx_stmt(uv_async_t * handle);
static void enter_xxx_columns(uv_async_t * handle);

static void exit_after_expr(uv_async_t * handle);
static void exit_alter_user(uv_async_t * handle);
static void exit_before_expr(uv_async_t * handle);
static void exit_between_expr(uv_async_t * handle);
static void exit_calc_stmt(uv_async_t * handle);
static void exit_count_pools_stmt(uv_async_t * handle);
static void exit_count_series_stmt(uv_async_t * handle);
static void exit_count_servers_stmt(uv_async_t * handle);
static void exit_count_users_stmt(uv_async_t * handle);
static void exit_create_user_stmt(uv_async_t * handle);
static void exit_drop_series_stmt(uv_async_t * handle);
static void exit_drop_shard_stmt(uv_async_t * handle);
static void exit_drop_user_stmt(uv_async_t * handle);
static void exit_grant_user_stmt(uv_async_t * handle);
static void exit_list_pools_stmt(uv_async_t * handle);
static void exit_list_series_stmt(uv_async_t * handle);
static void exit_list_servers_stmt(uv_async_t * handle);
static void exit_list_users_stmt(uv_async_t * handle);
static void exit_revoke_user_stmt(uv_async_t * handle);
static void exit_select_stmt(uv_async_t * handle);
static void exit_set_log_level(uv_async_t * handle);
static void exit_show_stmt(uv_async_t * handle);
static void exit_timeit_stmt(uv_async_t * handle);

static void on_count_servers_response(slist_t * promises, uv_async_t * handle);
static void on_list_xxx_response(slist_t * promises, uv_async_t * handle);

static uint32_t GID_K_NAME = CLERI_GID_K_NAME;
static uint32_t GID_K_POOL = CLERI_GID_K_POOL;
static uint32_t GID_K_VERSION = CLERI_GID_K_VERSION;
static uint32_t GID_K_ONLINE = CLERI_GID_K_ONLINE;
static uint32_t GID_K_STATUS = CLERI_GID_K_STATUS;
static uint32_t GID_K_SERVERS = CLERI_GID_K_SERVERS;
static uint32_t GID_K_SERIES = CLERI_GID_K_SERIES;

#define SIRIPARSER_NEXT_NODE                                                \
siridb_nodes_next(&query->nodes);                                           \
if (query->nodes == NULL)                                                   \
    siridb_send_query_result(handle);                                       \
else                                                                        \
{                                                                           \
    uv_async_t * forward = (uv_async_t *) malloc(sizeof(uv_async_t));       \
    forward->data = (void *) handle->data;                                  \
    uv_async_init(siri.loop, forward, (uv_async_cb) query->nodes->cb);      \
    uv_async_send(forward);                                                 \
    uv_close((uv_handle_t *) handle, (uv_close_cb) free);                   \
}

#define SIRIPARSER_MASTER_CHECK_ACCESS(ACCESS_BIT)                          \
if (    IS_MASTER &&                                                        \
        !siridb_user_check_access(                                          \
            ((sirinet_socket_t *) query->client->data)->origin,             \
            ACCESS_BIT,                                                     \
            query->err_msg))                                                \
    return siridb_send_error(handle, SN_MSG_QUERY_ERROR);


void siriparser_init_listener(void)
{
    for (uint_fast16_t i = 0; i < CLERI_END; i++)
    {
        siriparser_listen_enter[i] = NULL;
        siriparser_listen_exit[i] = NULL;
    }

    siriparser_listen_enter[CLERI_GID_ACCESS_EXPR] = enter_access_expr;
    siriparser_listen_enter[CLERI_GID_ALTER_SERVER] = enter_alter_server;
    siriparser_listen_enter[CLERI_GID_ALTER_USER] = enter_alter_user;
    siriparser_listen_enter[CLERI_GID_COUNT_STMT] = enter_count_stmt;
    siriparser_listen_enter[CLERI_GID_CREATE_USER_STMT] = enter_create_user_stmt;
    siriparser_listen_enter[CLERI_GID_DROP_STMT] = enter_drop_stmt;
    siriparser_listen_enter[CLERI_GID_GRANT_STMT] = enter_grant_stmt;
    siriparser_listen_enter[CLERI_GID_GRANT_USER_STMT] = enter_grant_user_stmt;
    siriparser_listen_enter[CLERI_GID_LIMIT_EXPR] = enter_limit_expr;
    siriparser_listen_enter[CLERI_GID_LIST_STMT] = enter_list_stmt;
    siriparser_listen_enter[CLERI_GID_POOL_COLUMNS] = enter_xxx_columns;
    siriparser_listen_enter[CLERI_GID_REVOKE_STMT] = enter_revoke_stmt;
    siriparser_listen_enter[CLERI_GID_REVOKE_USER_STMT] = enter_revoke_user_stmt;
    siriparser_listen_enter[CLERI_GID_SELECT_STMT] = enter_select_stmt;
    siriparser_listen_enter[CLERI_GID_SET_PASSWORD] = enter_set_password;
    siriparser_listen_enter[CLERI_GID_SERIES_COLUMNS] = enter_xxx_columns;
    siriparser_listen_enter[CLERI_GID_SERVER_COLUMNS] = enter_xxx_columns;
    siriparser_listen_enter[CLERI_GID_SERIES_NAME] = enter_series_name;
    siriparser_listen_enter[CLERI_GID_SERIES_MATCH] = enter_series_match;
    siriparser_listen_enter[CLERI_GID_TIMEIT_STMT] = enter_timeit_stmt;
    siriparser_listen_enter[CLERI_GID_USER_COLUMNS] = enter_xxx_columns;
    siriparser_listen_enter[CLERI_GID_WHERE_POOL_STMT] = enter_where_xxx_stmt;
    siriparser_listen_enter[CLERI_GID_WHERE_SERIES_STMT] = enter_where_xxx_stmt;
    siriparser_listen_enter[CLERI_GID_WHERE_SERVER_STMT] = enter_where_xxx_stmt;
    siriparser_listen_enter[CLERI_GID_WHERE_USER_STMT] = enter_where_xxx_stmt;


    siriparser_listen_exit[CLERI_GID_AFTER_EXPR] = exit_after_expr;
    siriparser_listen_exit[CLERI_GID_ALTER_USER] = exit_alter_user;
    siriparser_listen_exit[CLERI_GID_BEFORE_EXPR] = exit_before_expr;
    siriparser_listen_exit[CLERI_GID_BETWEEN_EXPR] = exit_between_expr;
    siriparser_listen_exit[CLERI_GID_CALC_STMT] = exit_calc_stmt;
    siriparser_listen_exit[CLERI_GID_COUNT_POOLS_STMT] = exit_count_pools_stmt;
    siriparser_listen_exit[CLERI_GID_COUNT_SERIES_STMT] = exit_count_series_stmt;
    siriparser_listen_exit[CLERI_GID_COUNT_SERVERS_STMT] = exit_count_servers_stmt;
    siriparser_listen_exit[CLERI_GID_COUNT_USERS_STMT] = exit_count_users_stmt;
    siriparser_listen_exit[CLERI_GID_CREATE_USER_STMT] = exit_create_user_stmt;
    siriparser_listen_exit[CLERI_GID_DROP_SERIES_STMT] = exit_drop_series_stmt;
    siriparser_listen_exit[CLERI_GID_DROP_SHARD_STMT] = exit_drop_shard_stmt;
    siriparser_listen_exit[CLERI_GID_DROP_USER_STMT] = exit_drop_user_stmt;
    siriparser_listen_exit[CLERI_GID_GRANT_USER_STMT] = exit_grant_user_stmt;
    siriparser_listen_exit[CLERI_GID_LIST_POOLS_STMT] = exit_list_pools_stmt;
    siriparser_listen_exit[CLERI_GID_LIST_SERIES_STMT] = exit_list_series_stmt;
    siriparser_listen_exit[CLERI_GID_LIST_SERVERS_STMT] = exit_list_servers_stmt;
    siriparser_listen_exit[CLERI_GID_LIST_USERS_STMT] = exit_list_users_stmt;
    siriparser_listen_exit[CLERI_GID_REVOKE_USER_STMT] = exit_revoke_user_stmt;
    siriparser_listen_exit[CLERI_GID_SELECT_STMT] = exit_select_stmt;
    siriparser_listen_exit[CLERI_GID_SET_LOG_LEVEL] = exit_set_log_level;
    siriparser_listen_exit[CLERI_GID_SHOW_STMT] = exit_show_stmt;
    siriparser_listen_exit[CLERI_GID_TIMEIT_STMT] = exit_timeit_stmt;
}

/******************************************************************************
 * Free functions
 *****************************************************************************/

static void decref_server_object(uv_handle_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    siridb_server_decref((siridb_server_t *) query->data);

    /* normal free call */
    siridb_query_free(handle);
}

static void decref_user_object(uv_handle_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    siridb_user_decref((siridb_user_t *) query->data);

    /* normal free call */
    siridb_query_free(handle);
}

/******************************************************************************
 * Enter functions
 *****************************************************************************/

static void enter_access_expr(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    /* bind ACCESS_EXPR children to query */
    query->data = query->nodes->node->children;

    SIRIPARSER_NEXT_NODE
}

static void enter_alter_server(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    SIRIPARSER_MASTER_CHECK_ACCESS(SIRIDB_ACCESS_ALTER)

    siridb_t * siridb = ((sirinet_socket_t *) query->client->data)->siridb;
    cleri_node_t * server_node =
                    query->nodes->node->children->next->node->children->node;

    siridb_server_t * server;

    switch (server_node->cl_obj->tp)
    {
    case CLERI_TP_CHOICE:  // server name
        {
            char name[server_node->len - 1];
            strx_extract_string(name, server_node->str, server_node->len);
            server = siridb_servers_by_name(siridb->servers, name);
        }
        break;

    case CLERI_TP_REGEX:  // uuid
        {
            uuid_t uuid;
            char * str_uuid = strndup(server_node->str, server_node->len);
            server = (uuid_parse(str_uuid, uuid) == 0) ?
                    siridb_servers_by_uuid(siridb->servers, uuid) : NULL;
            free(str_uuid);
        }
        break;

    default:
        /* we should NEVER get here */
        assert (0);
        break;
    }

    if (server == NULL)
    {
        snprintf(query->err_msg,
                SIRIDB_MAX_SIZE_ERR_MSG,
                "Cannot find server: %.*s",
                (int) server_node->len,
                server_node->str);
        return siridb_send_error(handle, SN_MSG_QUERY_ERROR);
    }

    query->data = server;
    siridb_server_incref(server);
    query->free_cb = (uv_close_cb) decref_server_object;

    SIRIPARSER_NEXT_NODE
}

static void enter_alter_user(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    SIRIPARSER_MASTER_CHECK_ACCESS(SIRIDB_ACCESS_ALTER)

    siridb_t * siridb = ((sirinet_socket_t *) query->client->data)->siridb;
    cleri_node_t * user_node =
                query->nodes->node->children->next->node;
    siridb_user_t * user;

    char username[user_node->len - 1];
    strx_extract_string(username, user_node->str, user_node->len);

    if ((user = siridb_users_get_user(siridb->users, username, NULL)) == NULL)
    {
        snprintf(query->err_msg,
                SIRIDB_MAX_SIZE_ERR_MSG,
                "Cannot find user: '%s'",
                username);
        return siridb_send_error(handle, SN_MSG_QUERY_ERROR);
    }

    query->data = user;
    siridb_user_incref(user);
    query->free_cb = (uv_close_cb) decref_user_object;

    SIRIPARSER_NEXT_NODE
}

static void enter_count_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    SIRIPARSER_MASTER_CHECK_ACCESS(SIRIDB_ACCESS_COUNT)

#ifdef DEBUG
    assert (query->packer == NULL);
#endif

    query->packer = qp_new_packer(256);
    qp_add_type(query->packer, QP_MAP_OPEN);

    query->data = query_count_new();
    query->free_cb = (uv_close_cb) query_count_free;

    SIRIPARSER_NEXT_NODE
}

static void enter_create_user_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    SIRIPARSER_MASTER_CHECK_ACCESS(SIRIDB_ACCESS_CREATE)

    /* bind user object to data and set correct free call */
    query->data = siridb_user_new();
    siridb_user_incref(query->data);

    query->free_cb = (uv_close_cb) decref_user_object;

    SIRIPARSER_NEXT_NODE
}

static void enter_drop_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

#ifdef DEBUG
    assert (query->packer == NULL);
#endif

    query->packer = qp_new_packer(1024);
    qp_add_type(query->packer, QP_MAP_OPEN);

    query->data = query_drop_new();
    query->free_cb = (uv_close_cb) query_drop_free;

    SIRIPARSER_MASTER_CHECK_ACCESS(SIRIDB_ACCESS_DROP)
    SIRIPARSER_NEXT_NODE
}

static void enter_grant_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    SIRIPARSER_MASTER_CHECK_ACCESS(SIRIDB_ACCESS_GRANT)
    SIRIPARSER_NEXT_NODE
}

static void enter_grant_user_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    siridb_t * siridb = ((sirinet_socket_t *) query->client->data)->siridb;
    cleri_node_t * user_node =
                query->nodes->node->children->next->node;
    siridb_user_t * user;
    char username[user_node->len - 1];
    strx_extract_string(username, user_node->str, user_node->len);

    if ((user = siridb_users_get_user(siridb->users, username, NULL)) == NULL)
    {
        snprintf(query->err_msg, SIRIDB_MAX_SIZE_ERR_MSG,
                "Cannot find user: '%s'", username);
        return siridb_send_error(handle, SN_MSG_QUERY_ERROR);
    }

    user->access_bit |=
            siridb_access_from_children((cleri_children_t *) query->data);

    query->data = user;
    siridb_user_incref(user);
    query->free_cb = (uv_close_cb) decref_user_object;

    SIRIPARSER_NEXT_NODE
}

static void enter_limit_expr(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    query_list_t * qlist = (query_list_t *) query->data;
    int64_t limit = query->nodes->node->children->next->node->result;

    if (limit <= 0)
    {
        snprintf(query->err_msg, SIRIDB_MAX_SIZE_ERR_MSG,
                "Limit must be a value larger than zero but received: '%ld'",
                limit);
        return siridb_send_error(handle, SN_MSG_QUERY_ERROR);
    }

    qlist->limit = limit;

    SIRIPARSER_NEXT_NODE
}

static void enter_list_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    SIRIPARSER_MASTER_CHECK_ACCESS(SIRIDB_ACCESS_LIST)

#ifdef DEBUG
    assert (query->packer == NULL);
#endif

    query->packer = qp_new_packer(QP_SUGGESTED_SIZE);
    qp_add_type(query->packer, QP_MAP_OPEN);

    qp_add_raw(query->packer, "columns", 7);
    qp_add_type(query->packer, QP_ARRAY_OPEN);

    query->data = query_list_new();
    query->free_cb = (uv_close_cb) query_list_free;

    SIRIPARSER_NEXT_NODE
}

static void enter_revoke_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    SIRIPARSER_MASTER_CHECK_ACCESS(SIRIDB_ACCESS_REVOKE)
    SIRIPARSER_NEXT_NODE
}

static void enter_revoke_user_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    siridb_t * siridb = ((sirinet_socket_t *) query->client->data)->siridb;
    cleri_node_t * user_node =
                query->nodes->node->children->next->node;
    siridb_user_t * user;
    char username[user_node->len - 1];
    strx_extract_string(username, user_node->str, user_node->len);

    if ((user = siridb_users_get_user(siridb->users, username, NULL)) == NULL)
    {
        snprintf(query->err_msg,
                SIRIDB_MAX_SIZE_ERR_MSG,
                "Cannot find user: '%s'",
                username);
        return siridb_send_error(handle, SN_MSG_QUERY_ERROR);
    }

    user->access_bit ^= (
            user->access_bit &
            siridb_access_from_children((cleri_children_t *) query->data));

    query->data = user;
    siridb_user_incref(user);
    query->free_cb = (uv_close_cb) decref_user_object;

    SIRIPARSER_NEXT_NODE
}

static void enter_select_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    SIRIPARSER_MASTER_CHECK_ACCESS(SIRIDB_ACCESS_SELECT)

#ifdef DEBUG
    assert (query->packer == NULL && query->data == NULL);
#endif

    query->data = query_select_new();
    query->free_cb = (uv_close_cb) query_select_free;

    query->packer = qp_new_packer(QP_SUGGESTED_SIZE);
    qp_add_type(query->packer, QP_MAP_OPEN);

    SIRIPARSER_NEXT_NODE
}

static void enter_set_password(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    siridb_user_t * user = (siridb_user_t *) query->data;
    cleri_node_t * pw_node =
            query->nodes->node->children->next->next->node;

    char password[pw_node->len - 1];
    strx_extract_string(password, pw_node->str, pw_node->len);

    if (siridb_user_set_password(user, password, query->err_msg))
    {
        return siridb_send_error(handle, SN_MSG_QUERY_ERROR);
    }

    SIRIPARSER_NEXT_NODE
}

static void enter_series_name(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    cleri_node_t * node = query->nodes->node;
    siridb_t * siridb = ((sirinet_socket_t *) query->client->data)->siridb;
    siridb_series_t * series;
    uint16_t pool;
    char series_name[node->len - 1];

    /* extract series name */
    strx_extract_string(series_name, node->str, node->len);

    /* get pool for series name */
    pool = siridb_pool_sn(siridb, series_name);

    /* check if this series belongs to 'this' pool and if so get the series */
    if (pool == siridb->server->pool)
    {
        if ((series = ct_get(siridb->series, series_name)) == NULL)
        {
            /* the series does not exist */
            snprintf(query->err_msg, SIRIDB_MAX_SIZE_ERR_MSG,
                    "Cannot find series: '%s'", series_name);

            /* free series_name and return with send_errror.. */
            return siridb_send_error(handle, SN_MSG_QUERY_ERROR);
        }

        /* bind the series to the query, increment ref count if successful */
        if (ct_add(((query_wrapper_ct_series_t *) query->data)->ct_series,
                series_name,
                series) == CT_OK)
        {
            siridb_series_incref(series);
        }
    }

    SIRIPARSER_NEXT_NODE
}

static void enter_series_match(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    ((query_wrapper_ct_series_t *) query->data)->ct_series = ct_new();

    SIRIPARSER_NEXT_NODE
}

static void enter_timeit_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    query->timeit = qp_new_packer(512);

    qp_add_raw(query->timeit, "__timeit__", 10);
    qp_add_type(query->timeit, QP_ARRAY_OPEN);

    SIRIPARSER_NEXT_NODE
}

static void enter_where_xxx_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    cexpr_t * cexpr =
            cexpr_from_node(query->nodes->node->children->next->node);

    if (cexpr == NULL)
    {
        sprintf(query->err_msg, "Max depth reached in 'where' expression!");
        log_critical(query->err_msg);
        return siridb_send_error(handle, SN_MSG_QUERY_ERROR);
    }
    else
    {
        ((query_wrapper_where_node_t *) query->data)->where_expr = cexpr;
    }

    SIRIPARSER_NEXT_NODE
}

static void enter_xxx_columns(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    cleri_children_t * columns = query->nodes->node->children;
    query_list_t * qlist = (query_list_t *) query->data;

    qlist->props = slist_new(DEFAULT_ALLOC_COLUMNS);

    while (1)
    {
        qp_add_raw(query->packer, columns->node->str, columns->node->len);

        slist_append_save(
                &qlist->props,
                &columns->node->children->node->cl_obj->cl_obj->dummy->gid);

        if (columns->next == NULL)
        {
            break;
        }

        columns = columns->next->next;
    }

    SIRIPARSER_NEXT_NODE
}

/******************************************************************************
 * Exit functions
 *****************************************************************************/

static void exit_after_expr(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    ((query_select_t *) query->data)->start_ts =
            (uint64_t *) &query->nodes->node->children->next->node->result;

    SIRIPARSER_NEXT_NODE
}

static void exit_alter_user(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    if (siridb_users_save(((sirinet_socket_t *) query->client->data)->siridb))
    {
        sprintf(query->err_msg, "Could not write users to file!");
        log_critical(query->err_msg);
        return siridb_send_error(handle, SN_MSG_QUERY_ERROR);
    }

    query->packer = qp_new_packer(1024);
    qp_add_type(query->packer, QP_MAP_OPEN);

    QP_ADD_SUCCESS
    qp_add_fmt(query->packer,
            "Successful changed password for user '%s'.",
            ((siridb_user_t *) query->data)->username);

    SIRIPARSER_NEXT_NODE
}

static void exit_before_expr(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    ((query_select_t *) query->data)->end_ts =
            (uint64_t *) &query->nodes->node->children->next->node->result;

    SIRIPARSER_NEXT_NODE
}

static void exit_between_expr(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    ((query_select_t *) query->data)->start_ts = (uint64_t *)
            &query->nodes->node->children->next->node->result;

    ((query_select_t *) query->data)->end_ts = (uint64_t *)
            &query->nodes->node->children->next->next->next->node->result;

    SIRIPARSER_NEXT_NODE
}

static void exit_calc_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    siridb_t * siridb = ((sirinet_socket_t *) query->client->data)->siridb;
    cleri_node_t * calc_node = query->nodes->node->children->node;

    query->packer = qp_new_packer(64);
    qp_add_type(query->packer, QP_MAP_OPEN);
    qp_add_raw(query->packer, "calc", 4);

    if (query->time_precision == SIRIDB_TIME_DEFAULT)
        qp_add_int64(query->packer, calc_node->result);
    else
    {
        double factor =
                pow(1000.0, query->time_precision - siridb->time->precision);
        qp_add_int64(query->packer, (int64_t) (calc_node->result * factor));
    }

    SIRIPARSER_NEXT_NODE
}

static void exit_count_pools_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    siridb_t * siridb = ((sirinet_socket_t *) query->client->data)->siridb;
    cexpr_t * where_expr = ((query_count_t *) query->data)->where_expr;

    qp_add_raw(query->packer, "pools", 5);

    if (where_expr == NULL)
    {
        qp_add_int64(query->packer, siridb->pools->size);
    }
    else
    {
        int n = 0;
        siridb_pool_walker_t wpool;
        cexpr_cb_t cb = (cexpr_cb_t) siridb_pool_cexpr_cb;
        for (wpool.pid = 0; wpool.pid < siridb->pools->size; wpool.pid++)
        {
            wpool.servers = siridb->pools->pool[wpool.pid].size;
            wpool.series = siridb->series->len;
            n += cexpr_run(where_expr, cb, &wpool);
        }
        qp_add_int64(query->packer, n);
    }
    SIRIPARSER_NEXT_NODE
}

static void exit_count_series_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    siridb_t * siridb = ((sirinet_socket_t *) query->client->data)->siridb;

    qp_add_raw(query->packer, "series", 6);
    qp_add_int64(query->packer, siridb->series_map->len);

    SIRIPARSER_NEXT_NODE
}

static void exit_count_servers_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    siridb_t * siridb = ((sirinet_socket_t *) query->client->data)->siridb;
    cexpr_t * where_expr = ((query_list_t *) query->data)->where_expr;
    query_count_t * q_count = (query_count_t *) query->data;
    cexpr_cb_t cb = (cexpr_cb_t) siridb_server_cexpr_cb;

    qp_add_raw(query->packer, "servers", 7);

    int is_local = IS_MASTER;

    /* if is_local, check if we use 'remote' props in where expression */
    if (is_local && where_expr != NULL)
    {
        is_local = !cexpr_contains(where_expr, siridb_server_is_remote_prop);
    }

    if (is_local)
    {
        for (   llist_node_t * node = siridb->servers->first;
                node != NULL;
                node = node->next)
        {
            siridb_server_walker_t wserver = {node->data, siridb};
            if (where_expr == NULL || cexpr_run(where_expr, cb, &wserver))
            {
                q_count->n++;
            }
        }
    }
    else
    {
        siridb_server_walker_t wserver = {siridb->server, siridb};
        if (where_expr == NULL || cexpr_run(where_expr, cb, &wserver))
        {
            q_count->n++;
        }
    }

    if (IS_MASTER && !is_local)
    {
        siridb_query_forward(
                handle,
                BP_QUERY_SERVER,
                (sirinet_promises_cb_t) on_count_servers_response);
    }
    else
    {
        qp_add_int64(query->packer, q_count->n);
        SIRIPARSER_NEXT_NODE
    }
}

static void exit_count_users_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    siridb_t * siridb = ((sirinet_socket_t *) query->client->data)->siridb;
    llist_node_t * node = siridb->users->first;
    cexpr_t * where_expr = ((query_count_t *) query->data)->where_expr;
    cexpr_cb_t cb = (cexpr_cb_t) siridb_user_cexpr_cb;
    int n = 0;

    qp_add_raw(query->packer, "users", 5);

    while (node != NULL)
    {
        if (where_expr == NULL || cexpr_run(where_expr, cb, node->data))
        {
            n++;
        }
        node = node->next;
    }

    qp_add_int64(query->packer, n);

    SIRIPARSER_NEXT_NODE
}

static void exit_create_user_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    siridb_user_t * user = (siridb_user_t *) query->data;
    cleri_node_t * user_node =
            query->nodes->node->children->next->node;

#ifdef DEBUG
    /* both username and packer should be NULL at this point */
    assert(user->username == NULL);
    assert(query->packer == NULL);
#endif

    user->username = (char *) malloc(user_node->len - 1);
    strx_extract_string(user->username, user_node->str, user_node->len);

    if (siridb_users_add_user(
            ((sirinet_socket_t *) query->client->data)->siridb,
            user,
            query->err_msg))
    {
        return siridb_send_error(handle, SN_MSG_QUERY_ERROR);
    }

    /* success, we do not need to free the user anymore */
    query->free_cb = (uv_close_cb) siridb_query_free;

    query->packer = qp_new_packer(1024);
    qp_add_type(query->packer, QP_MAP_OPEN);

    QP_ADD_SUCCESS
    qp_add_fmt(query->packer,
            "User '%s' is created successfully.", user->username);
    SIRIPARSER_NEXT_NODE
}

static void exit_drop_series_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    siridb_t * siridb = ((sirinet_socket_t *) query->client->data)->siridb;

    query_drop_t * q_drop = (query_drop_t *) query->data;

    uv_mutex_lock(&siridb->series_mutex);

    ct_walk(q_drop->ct_series,
            (ct_cb_t) &walk_drop_series, handle);

    uv_mutex_unlock(&siridb->series_mutex);

    /* flush dropped file change to disk */
    fflush(siridb->dropped_fp);

    QP_ADD_SUCCESS
    qp_add_fmt(query->packer,
            "Successfully dropped %ld series.", q_drop->ct_series->len);

    SIRIPARSER_NEXT_NODE
}

static void exit_drop_shard_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    cleri_node_t * shard_id_node =
                query->nodes->node->children->next->node;
    siridb_t * siridb = ((sirinet_socket_t *) query->client->data)->siridb;

    int64_t shard_id = atoll(shard_id_node->str);

    uv_mutex_lock(&siridb->shards_mutex);

    siridb_shard_t * shard = imap64_pop(siridb->shards, shard_id);

    uv_mutex_unlock(&siridb->shards_mutex);

    if (shard == NULL)
    {
        log_debug(
                "Cannot find shard '%ld' on server '%s'",
                shard_id,
                siridb->server->name);
    }
    else
    {
        ((query_drop_t *) query->data)->data = shard;

        /* We need a series mutex here since we depend on the series index */
        uv_mutex_lock(&siridb->series_mutex);

        imap32_walk(
                siridb->series_map,
                (imap32_cb_t) walk_drop_shard,
                (void *) handle);

        uv_mutex_unlock(&siridb->series_mutex);

        shard->flags |= SIRIDB_SHARD_WILL_BE_REMOVED;

        siridb_shard_decref(shard);

    }

    /* we send back a successful message even when the shard was not found
     * because it might be dropped on another server so at least the shard
     * is gone.
     */
    QP_ADD_SUCCESS
    qp_add_fmt(query->packer,
            "Shard '%ld' is dropped successfully.", shard_id);

    SIRIPARSER_NEXT_NODE
}

static void exit_drop_user_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    cleri_node_t * user_node =
            query->nodes->node->children->next->node;
    char username[user_node->len - 1];

    /* we need to free user-name */
    strx_extract_string(username, user_node->str, user_node->len);

    if (siridb_users_drop_user(
            ((sirinet_socket_t *) query->client->data)->siridb,
            username,
            query->err_msg))
    {
        return siridb_send_error(handle, SN_MSG_QUERY_ERROR);
    }
    QP_ADD_SUCCESS
    qp_add_fmt(query->packer,
            "User '%s' is dropped successfully.", username);

    SIRIPARSER_NEXT_NODE
}

static void exit_grant_user_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    if (siridb_users_save(((sirinet_socket_t *) query->client->data)->siridb))
    {
        sprintf(query->err_msg, "Could not write users to file!");
        log_critical(query->err_msg);
        return siridb_send_error(handle, SN_MSG_QUERY_ERROR);
    }

#ifdef DEBUG
    assert (query->packer == NULL);
#endif

    query->packer = qp_new_packer(1024);
    qp_add_type(query->packer, QP_MAP_OPEN);

    QP_ADD_SUCCESS
    qp_add_fmt(query->packer,
            "Successfully granted permissions to user '%s'.",
            ((siridb_user_t *) query->data)->username);

    SIRIPARSER_NEXT_NODE
}

static void exit_list_pools_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    siridb_t * siridb = ((sirinet_socket_t *) query->client->data)->siridb;
    query_list_t * qlist = (query_list_t *) query->data;
    siridb_pool_t * pool;
    siridb_pool_walker_t wpool;
    uint_fast16_t prop, n;
    cexpr_t * where_expr = ((query_list_t *) query->data)->where_expr;
    cexpr_cb_t cb = (cexpr_cb_t) siridb_pool_cexpr_cb;

    if (qlist->props == NULL)
    {
        qlist->props = slist_new(3);
        slist_append(qlist->props, &GID_K_POOL);
        slist_append(qlist->props, &GID_K_SERVERS);
        slist_append(qlist->props, &GID_K_SERIES);
        qp_add_raw(query->packer, "pool", 4);
        qp_add_raw(query->packer, "servers", 7);
        qp_add_raw(query->packer, "series", 6);
    }

    qp_add_type(query->packer, QP_ARRAY_CLOSE);

    qp_add_raw(query->packer, "pools", 5);
    qp_add_type(query->packer, QP_ARRAY_OPEN);

    for (   wpool.pid = 0, n = 0;
            wpool.pid < siridb->pools->size && n < qlist->limit;
            wpool.pid++)
    {
        pool = siridb->pools->pool + wpool.pid;

        wpool.servers = pool->size;
        wpool.series = siridb->series->len;

        if (where_expr == NULL || cexpr_run(where_expr, cb, &wpool))
        {
            qp_add_type(query->packer, QP_ARRAY_OPEN);

            for (prop = 0; prop < qlist->props->len; prop++)
            {
                switch(*((uint32_t *) qlist->props->data[prop]))
                {
                case CLERI_GID_K_POOL:
                    qp_add_int16(query->packer, wpool.pid);
                    break;
                case CLERI_GID_K_SERVERS:
                    qp_add_int16(query->packer, wpool.servers);
                    break;
                case CLERI_GID_K_SERIES:
                    qp_add_int64(query->packer, wpool.series);
                    break;
                }
            }

            qp_add_type(query->packer, QP_ARRAY_CLOSE);
            n++;
        }
    }

    qp_add_type(query->packer, QP_ARRAY_CLOSE);

    SIRIPARSER_NEXT_NODE
}

static void exit_list_series_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    siridb_t * siridb = ((sirinet_socket_t *) query->client->data)->siridb;
    query_list_t * q_list = (query_list_t *) query->data;

    if (q_list->props == NULL)
    {
        q_list->props = slist_new(1);
        slist_append(q_list->props, &GID_K_NAME);
        qp_add_raw(query->packer, "name", 4);
    }

    qp_add_type(query->packer, QP_ARRAY_CLOSE);

    qp_add_raw(query->packer, "series", 6);
    qp_add_type(query->packer, QP_ARRAY_OPEN);

    /* We only need the mutex for sure when accessing some properties for the
     * series and when accessing the main series tree although this tree will
     * not be changed by the optimize thread.
     */
    uv_mutex_lock(&siridb->series_mutex);

    ct_walkn(
            (q_list->ct_series == NULL) ? siridb->series : q_list->ct_series,
            &q_list->limit,
            (ct_cb_t) &walk_list_series,
            handle);

    uv_mutex_unlock(&siridb->series_mutex);

    if (IS_MASTER && q_list->limit)
    {
        /* we have not reached the limit, send the query to oter pools */
        siridb_query_forward(
                handle,
                BP_QUERY_POOL,
                (sirinet_promises_cb_t) on_list_xxx_response);
    }
    else
    {
        qp_add_type(query->packer, QP_ARRAY_CLOSE);

        SIRIPARSER_NEXT_NODE
    }
}

static void exit_list_servers_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    siridb_t * siridb = ((sirinet_socket_t *) query->client->data)->siridb;
    cexpr_t * where_expr = ((query_list_t *) query->data)->where_expr;
    query_list_t * q_list = (query_list_t *) query->data;
    int is_local = IS_MASTER;

    /* if is_local, check if we need ask for 'remote' columns */
    if (is_local && q_list->props != NULL)
    {
        for (int i = 0; i < q_list->props->len; i++)
        {
            if (siridb_server_is_remote_prop(
                    *((uint32_t *) q_list->props->data[i])))
            {
                is_local = 0;
                break;
            }
        }
    }

    /* if is_local, check if we use 'remote' props in where expression */
    if (is_local && where_expr != NULL)
    {
        is_local = !cexpr_contains(where_expr, siridb_server_is_remote_prop);
    }

    if (q_list->props == NULL)
    {
        q_list->props = slist_new(5);
        slist_append(q_list->props, &GID_K_NAME);
        slist_append(q_list->props, &GID_K_POOL);
        slist_append(q_list->props, &GID_K_VERSION);
        slist_append(q_list->props, &GID_K_ONLINE);
        slist_append(q_list->props, &GID_K_STATUS);
        qp_add_raw(query->packer, "name", 4);
        qp_add_raw(query->packer, "pool", 4);
        qp_add_raw(query->packer, "version", 7);
        qp_add_raw(query->packer, "online", 6);
        qp_add_raw(query->packer, "status", 6);
    }

    qp_add_type(query->packer, QP_ARRAY_CLOSE);

    qp_add_raw(query->packer, "servers", 7);
    qp_add_type(query->packer, QP_ARRAY_OPEN);

    if (is_local)
    {
        llist_walkn(
                siridb->servers,
                &q_list->limit,
                (llist_cb_t) walk_list_servers,
                handle);
    }
    else
    {
        q_list->limit -= walk_list_servers(siridb->server, handle);
    }


    if (IS_MASTER && !is_local && q_list->limit)
    {
        siridb_query_forward(
                handle,
                BP_QUERY_SERVER,
                (sirinet_promises_cb_t) on_list_xxx_response);
    }
    else
    {
        qp_add_type(query->packer, QP_ARRAY_CLOSE);
        SIRIPARSER_NEXT_NODE
    }
}

static void exit_list_users_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    llist_node_t * node =
            ((sirinet_socket_t *) query->client->data)->siridb->users->first;
    slist_t * props = ((query_list_t *) query->data)->props;
    cexpr_t * where_expr = ((query_list_t *) query->data)->where_expr;
    cexpr_cb_t cb = (cexpr_cb_t) siridb_user_cexpr_cb;

    size_t i;
    siridb_user_t * user;

    if (props == NULL)
    {
        qp_add_raw(query->packer, "user", 4);
        qp_add_raw(query->packer, "access", 6);
    }

    qp_add_type(query->packer, QP_ARRAY_CLOSE);

    qp_add_raw(query->packer, "users", 5);
    qp_add_type(query->packer, QP_ARRAY_OPEN);

    while (node != NULL)
    {
        user = node->data;

        if (where_expr == NULL || cexpr_run(where_expr, cb, user))
        {
            qp_add_type(query->packer, QP_ARRAY_OPEN);

            if (props == NULL)
            {
                siridb_user_prop(user, query->packer, CLERI_GID_K_USER);
                siridb_user_prop(user, query->packer, CLERI_GID_K_ACCESS);
            }
            else
            {
                for (i = 0; i < props->len; i++)
                {
                    siridb_user_prop(
                            user,
                            query->packer,
                            *((uint32_t *) props->data[i]));
                }
            }

            qp_add_type(query->packer, QP_ARRAY_CLOSE);

        }
        node = node->next;
    }
    qp_add_type(query->packer, QP_ARRAY_CLOSE);

    SIRIPARSER_NEXT_NODE
}

static void exit_revoke_user_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    if (siridb_users_save(((sirinet_socket_t *) query->client->data)->siridb))
    {
        sprintf(query->err_msg, "Could not write users to file!");
        log_critical(query->err_msg);
        return siridb_send_error(handle, SN_MSG_QUERY_ERROR);
    }

#ifdef DEBUG
    assert (query->packer == NULL);
#endif

    query->packer = qp_new_packer(1024);
    qp_add_type(query->packer, QP_MAP_OPEN);

    QP_ADD_SUCCESS
    qp_add_fmt(query->packer,
            "Successfully revoked permissions from user '%s'.",
            ((siridb_user_t *) query->data)->username);

    SIRIPARSER_NEXT_NODE
}

static void exit_select_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    siridb_t * siridb = ((sirinet_socket_t *) query->client->data)->siridb;

    uv_mutex_lock(&siridb->series_mutex);

    ct_walk(((query_select_t *) query->data)->ct_series,
            (ct_cb_t) &walk_select, handle);

    uv_mutex_unlock(&siridb->series_mutex);

    SIRIPARSER_NEXT_NODE
}

static void exit_set_log_level(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    siridb_t * siridb = ((sirinet_socket_t *) query->client->data)->siridb;

#ifdef DEBUG
    assert (query->data != NULL);
    assert (IS_MASTER);
#endif

    siridb_server_t * server = query->data;
    cleri_node_t * node =
            query->nodes->node->children->next->next->node->children->node;

    int log_level;

    switch (node->cl_obj->cl_obj->keyword->gid)
    {
    case CLERI_GID_K_DEBUG:
        log_level = LOGGER_DEBUG;
        break;
    case CLERI_GID_K_INFO:
        log_level = LOGGER_INFO;
        break;
    case CLERI_GID_K_WARNING:
        log_level = LOGGER_WARNING;
        break;
    case CLERI_GID_K_ERROR:
        log_level = LOGGER_ERROR;
        break;
    case CLERI_GID_K_CRITICAL:
        log_level = LOGGER_CRITICAL;
        break;
    default:
        /* we should NEVER get here */
        assert(0);
        break;
    }
    if (server == siridb->server)
    {
        logger_set_level(log_level);
    }

    query->packer = qp_new_packer(1024);
    qp_add_type(query->packer, QP_MAP_OPEN);

    QP_ADD_SUCCESS
    qp_add_fmt(query->packer,
            "Successful set log level to '%s' on '%s'.",
            logger_level_name(log_level),
            server->name);

    SIRIPARSER_NEXT_NODE
}

static void exit_show_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;

    SIRIPARSER_MASTER_CHECK_ACCESS(SIRIDB_ACCESS_SHOW)

    cleri_children_t * children =
            query->nodes->node->children->next->node->children;
    siridb_props_cb prop_cb;

#ifdef DEBUG
    assert (query->packer == NULL);
#endif

    query->packer = qp_new_packer(4096);
    qp_add_type(query->packer, QP_MAP_OPEN);
    qp_add_raw(query->packer, "data", 4);
    qp_add_type(query->packer, QP_ARRAY_OPEN);

    siridb_user_t * user = ((sirinet_socket_t *) query->client->data)->origin;
    who_am_i = user->username;

    if (children->node == NULL)
    {
        /* show all properties */
        int i;

        for (i = 0; i < KW_COUNT; i++)
        {
            if ((prop_cb = siridb_props[i]) == NULL)
            {
                continue;
            }
            prop_cb(((sirinet_socket_t *) query->client->data)->siridb,
                    query->packer, 1);
        }
    }
    else
    {
        /* show selected properties chosen by query */
        while (1)
        {
            /* get the callback */
            prop_cb = siridb_props[children->node->children->
                                   node->cl_obj->cl_obj->
                                   keyword->gid - KW_OFFSET];
#ifdef DEBUG
            /* TODO: can be removed as soon as all props are implemented */
            if (prop_cb == NULL)
            {
                LOGC("not implemented");
            }
            else
#endif
            prop_cb(((sirinet_socket_t *) query->client->data)->siridb,
                    query->packer, 1);

            if (children->next == NULL)
            {
                break;
            }

            /* skip one which is the delimiter */
            children = children->next->next;
        }
    }

    qp_add_type(query->packer, QP_ARRAY_CLOSE);

    SIRIPARSER_NEXT_NODE
}

static void exit_timeit_stmt(uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    struct timespec end;

    clock_gettime(CLOCK_REALTIME, &end);

    qp_add_type(query->timeit, QP_MAP2);
    qp_add_raw(query->timeit, "server", 6);
    qp_add_string(
            query->timeit,
            ((sirinet_socket_t *) query->client->data)->siridb->server->name);
    qp_add_raw(query->timeit, "time", 4);
    qp_add_double(query->timeit,
            (double) (end.tv_sec - query->start.tv_sec) +
            (double) (end.tv_nsec - query->start.tv_nsec) / 1000000000.0f);

    if (query->packer == NULL)
    {
        /* lets give the new packer the exact size so we do not
         * need a realloc */
        query->packer = qp_new_packer(query->timeit->len + 1);
        qp_add_type(query->packer, QP_MAP_OPEN);
    }

    /* extend packer with timeit information */
    qp_extend_packer(query->packer, query->timeit);

    SIRIPARSER_NEXT_NODE
}

/******************************************************************************
 * On Response functions
 *****************************************************************************/

static void on_count_servers_response(slist_t * promises, uv_async_t * handle)
{
    siridb_query_t * query = (siridb_query_t *) handle->data;
    sirinet_pkg_t * pkg;
    sirinet_promise_t * promise;
    qp_unpacker_t * unpacker;
    qp_obj_t * qp_count = qp_new_object();
    query_count_t * q_count = query->data;

    for (size_t i = 0; i < promises->len; i++)
    {
        promise = promises->data[i];

        if (promise == NULL)
        {
            continue;
        }

        pkg = promise->data;

        if (pkg != NULL && pkg->tp == BP_QUERY_RESPONSE)
        {
            unpacker = qp_new_unpacker(pkg->data, pkg->len);

            if (    qp_is_map(qp_next(unpacker, NULL)) &&
                    qp_is_raw(qp_next(unpacker, NULL)) &&  // servers
                    qp_is_int(qp_next(unpacker, qp_count))) // one result
            {
                q_count->n += qp_count->via->int64;

                /* extract time-it info if needed */
                if (query->timeit != NULL)
                {
                    siridb_query_timeit_from_unpacker(query, unpacker);
                }
            }

            /* free the unpacker */
            qp_free_unpacker(unpacker);
        }

        /* make sure we free the promise and data */
        free(promise->data);
        free(promise);
    }

    qp_free_object(qp_count);
    qp_add_int64(query->packer, q_count->n);

    SIRIPARSER_NEXT_NODE
}


static void on_list_xxx_response(slist_t * promises, uv_async_t * handle)
{
    /*
     * Used for list_series, list_servers
     */
    siridb_query_t * query = (siridb_query_t *) handle->data;
    sirinet_pkg_t * pkg;
    sirinet_promise_t * promise;
    qp_unpacker_t * unpacker;
    query_list_t * q_list = (query_list_t *) query->data;

    for (size_t i = 0; i < promises->len; i++)
    {
        promise = promises->data[i];

        if (promise == NULL)
        {
            continue;
        }

        pkg = promise->data;

        if (pkg != NULL && pkg->tp == BP_QUERY_RESPONSE)
        {
            unpacker = qp_new_unpacker(pkg->data, pkg->len);

            if (    qp_is_map(qp_next(unpacker, NULL)) &&
                    qp_is_raw(qp_next(unpacker, NULL)) && // columns
                    qp_is_array(qp_skip_next(unpacker)) &&
                    qp_is_raw(qp_next(unpacker, NULL)) && // series/servers/...
                    qp_is_array(qp_next(unpacker, NULL)))  // holding results
            {
                while (qp_is_array(qp_current(unpacker)))
                {
                    if (q_list->limit)
                    {
                        qp_extend_from_unpacker(query->packer, unpacker);
                        q_list->limit--;
                    }
                    else
                    {
                        qp_skip_next(unpacker);
                    }
                }

                /* extract time-it info if needed */
                if (query->timeit != NULL)
                {
                    siridb_query_timeit_from_unpacker(query, unpacker);
                }

            }

            /* free the unpacker */
            qp_free_unpacker(unpacker);

        }

        /* make sure we free the promise and data */
        free(promise->data);
        free(promise);
    }

    qp_add_type(query->packer, QP_ARRAY_CLOSE);

    SIRIPARSER_NEXT_NODE
}

