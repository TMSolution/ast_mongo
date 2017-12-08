/*
 * MongoDB configuration engine
 *
 * Copyright: (c) 2015-2016 KINOSHITA minoru
 * License: GNU GENERAL PUBLIC LICENSE Version 2
 */

/*! \file
 *
 * \brief MongoDB configuration engine
 *
 * \author KINOSHITA minoru
 *
 * This is a realtime configuration engine for the MongoDB database
 * \ingroup resources
 *
 * Depends on the MongoDB library - https://www.mongodb.org
 *
 */

/*! \li \ref res_config_mongodb.c uses the configuration file \ref res_config_mongodb.conf
 * \addtogroup configuration_file Configuration Files
 */

/*** MODULEINFO
    <depend>res_mongodb</depend>
    <depend>mongoc</depend>
    <depend>bson</depend>
    <support_level>extended</support_level>
 ***/

#include "asterisk.h"
#ifdef ASTERISK_REGISTER_FILE   /* deprecated from 15.0.0 */
ASTERISK_REGISTER_FILE()
#endif

#include "asterisk/file.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/config.h"
#include "asterisk/module.h"
#include "asterisk/lock.h"
#include "asterisk/utils.h"
#include "asterisk/threadstorage.h"
#include "asterisk/res_mongodb.h"

#define BSON_UTF8_VALIDATE(utf8,allow_null) \
      bson_utf8_validate (utf8, (int) strlen (utf8), allow_null)

#define LOG_BSON_AS_JSON(level, fmt, bson, ...) { \
            size_t length;  \
            char *str = bson_as_json(bson, &length); \
            ast_log(level, fmt, str, ##__VA_ARGS__); \
            bson_free(str); \
        }

static const int MAXTOKENS = 3;
static const char NAME[] = "mongodb";
static const char CATEGORY[] = "mongodb";
static const char CONFIG_FILE[] = "res_config_mongodb.conf";
static const char SERVERID[] = "serverid";

AST_MUTEX_DEFINE_STATIC(model_lock);
static mongoc_client_pool_t* dbpool = NULL;
static bson_t* models = NULL;
static bson_oid_t *serverid = NULL;

static int str_split(char* str, const char* delim, const char* tokens[] ) {
    char* token;
    char* saveptr;
    int count = 0;

    for(token = strtok_r(str, delim, &saveptr);
        token && count < MAXTOKENS;
        token = strtok_r(NULL, delim, &saveptr), count++)
    {
        tokens[count] = token;
    }
    return count;
}

static const char *key_mongo2asterisk(const char *key)
{
    return strcmp(key, "_id") == 0 ? "id" : key;
}

static const char *key_asterisk2mongo(const char *key)
{
    return strcmp(key, "id") == 0 ? "_id" : key;
}

/*!
 *  check if the specified string is integer
 *
 *  \param[in] value
 *  \param[out] result
 *  \retval true if it's an integer
 */
static int is_integer(const char* pstr)
{
    size_t span = strspn(pstr, "0123456789");
    return span && pstr[span] == '\0';
}

/*!
 *  assume the specified src doesn't have any escaping letters for mongo such as \, ', ".
 *
 *  \retval a pointer same as dst.
 */
static const char* strcopy(const char* src, char* dst, int size)
{
    char* p = dst;
    int escaping  = 0;
    int i;
    for (i = 0; *src != '\0' && i < (size-1); ) {
        int c = *src++;
        if (escaping) {
            *p++ = c;
            ++i;
            escaping = 0;
        }
        else if (c == '%')
            break;
        else if (c == '\\')
            escaping = 1;
        else {
            *p++ = c;
            ++i;
        }
    }
    if (i == (size-1))
        ast_log(LOG_WARNING, "size of dst is not enough.\n");
    *p = '\0';
    return (const char*)dst;
}

/*!
 * \brief   make a condition to query
 * \param   sql     is pattern for sql
 * \retval  a bson to query as follows;
 *      sql patern      generated bson to query
 *      ----------      --------------------------------------
 *      %               { $exists: true, $not: { $size: 0} } }
 *      %patern%        { $regex: "patern" }
 *      patern%         { $regex: "^patern" }
 *      %patern         { $regex: "patern$" }
 *      any other       NULL
 */
static const bson_t* make_condition(const char* sql)
{
    bson_t* condition = NULL;
    char patern[1020];
    char tmp[1024];
    char head = *sql;
    char tail = *(sql + strlen(sql) - 1);

    if (strcmp(sql, "%") == 0) {
        const char* json = "{ \"$exists\": true, \"$not\": {\"$size\": 0}}";
        bson_error_t error;
        condition = bson_new_from_json((const uint8_t*)json, -1, &error);
        if (!condition)
            ast_log(LOG_ERROR, "cannot generated condition from \"%s\", %d.%d:%s\n", json, error.domain, error.code, error.message);
    }
    else if (head == '%' && tail == '%') {
        strcopy(sql+1, patern, sizeof(patern)-1);
        snprintf(tmp, sizeof(tmp), "%s", patern);
        condition = bson_new();
        BSON_APPEND_UTF8(condition, "$regex", tmp);
    }
    else if (head == '%') {
        strcopy(sql+1, patern, sizeof(patern)-1);
        snprintf(tmp, sizeof(tmp), "%s$", patern);
        condition = bson_new();
        BSON_APPEND_UTF8(condition, "$regex", tmp);
    }
    else if (tail == '%') {
        strcopy(sql, patern, sizeof(patern));
        snprintf(tmp, sizeof(tmp), "^%s", patern);
        condition = bson_new();
        BSON_APPEND_UTF8(condition, "$regex", tmp);
    }
    else {
        ast_log(LOG_WARNING, "not supported condition, \"%s\"\n", sql);
    }

    if (condition) {
        // LOG_BSON_AS_JSON(LOG_DEBUG, "generated condition is \"%s\"\n", condition);
    }
    else
        ast_log(LOG_WARNING, "no condition generated\n");

    return (const bson_t*)condition;
}

/*!
 * \brief make a query
 * \param fields
 * \param orderby
 * \retval  a bson object to query,
 * \retval  NULL if something wrong.
 */
static bson_t *make_query(const struct ast_variable *fields, const char *orderby)
{
    bson_t *root = NULL;
    bson_t *query = NULL;
    bson_t *order = NULL;

    do {
        bool err;

        query = serverid ? BCON_NEW(SERVERID, BCON_OID(serverid)) : bson_new();
        order = orderby ? BCON_NEW(key_asterisk2mongo(orderby), BCON_DOUBLE(1)) : bson_new();

        for(err = false; fields && !err; fields = fields->next) {
            const bson_t *condition = NULL;
            const char *tokens[MAXTOKENS];
            char buf[1024];
            int count;

            if (strlen(fields->name) >= (sizeof(buf) - 1)) {
                ast_log(LOG_WARNING, "too long key, \"%s\".\n", fields->name);
                continue;
            }
            strcpy(buf, fields->name);
            count = str_split(buf, " ", tokens);
            err = true;

            switch(count) {
                case 1:
                    err = !BSON_APPEND_UTF8(query, key_asterisk2mongo(fields->name), fields->value);
                    break;
                case 2:
                    if (!strcasecmp(tokens[1], "LIKE")) {
                        condition = make_condition(fields->value);
                    }
                    else if (!strcasecmp(tokens[1], "!=")) {
                        // {
                        //     tokens[0]: {
                        //         "$exists" : true,
                        //         "$ne" : value
                        //     }
                        // }
                        condition = BCON_NEW(
                            "$exists", BCON_BOOL(1),
                            "$ne", BCON_UTF8(fields->value)
                        );
                    }
                    else if (!strcasecmp(tokens[1], ">")) {
                        // {
                        //     tokens[0]: {
                        //         "$gt" : value
                        //     }
                        // }
                        if (is_integer(fields->value)) {
                            int32_t number = atoi(fields->value);
                            condition = BCON_NEW("$gt", BCON_INT32(number));
                        }
                        else {
                            condition = BCON_NEW("$gt", BCON_UTF8(fields->value));
                        }
                    }
                    else if (!strcasecmp(tokens[1], "<=")) {
                        // {
                        //     tokens[0]: {
                        //         "$lte" : value
                        //     }
                        // }
                        if (is_integer(fields->value)) {
                            int32_t number = atoi(fields->value);
                            condition = BCON_NEW("$lte", BCON_INT32(number));
                        }
                        else {
                            condition = BCON_NEW("$lte", BCON_UTF8(fields->value));
                        }
                    }
                    else {
                        ast_log(LOG_WARNING, "unexpected operator \"%s\" of \"%s\" \"%s\".\n", tokens[1], fields->name, fields->value);
                        break;
                    }
                    if (!condition) {
                        ast_log(LOG_ERROR, "something wrong.\n");
                        break;
                    }

                    err = !BSON_APPEND_DOCUMENT(query, key_asterisk2mongo(tokens[0]), condition);

                    break;
                default:
                    ast_log(LOG_WARNING, "not handled, name=%s, value=%s.\n", fields->name, fields->value);
            }
            if (condition)
                bson_destroy((bson_t*)condition);
            else if (count > 1) {
                ast_log(LOG_ERROR, "something wrong.\n");
                break;
            }
        }
        if (err) {
            ast_log(LOG_ERROR, "something wrong.\n");
            break;
        }
        root = BCON_NEW("$query", BCON_DOCUMENT(query),
                        "$orderby", BCON_DOCUMENT(order));
        if (!root) {    // current BCON_NEW might not return any error such as NULL...
            ast_log(LOG_WARNING, "not enough memory\n");
            break;
        }
    } while(0);
    if (query)
        bson_destroy(query);
    if (order)
        bson_destroy(order);
    // if (root) {
    //     LOG_BSON_AS_JSON(LOG_DEBUG, "generated query is %s\n", root);
    // }
    return root;
}

/*!
 * \param   model_name  is name of model to be retrieved.
 * \param   property
 * \retval  bson type
 */
static bson_type_t model_get_btype(const char* model_name, const char* property)
{
    bson_type_t btype = BSON_TYPE_UNDEFINED;
    bson_iter_t iroot;
    bson_iter_t imodel;

    ast_mutex_lock(&model_lock);
    do {
        if (bson_iter_init_find (&iroot, models, model_name) &&
            BSON_ITER_HOLDS_DOCUMENT (&iroot) &&
            bson_iter_recurse (&iroot, &imodel) &&
            bson_iter_find(&imodel, property))
        {
            btype = (bson_type_t)bson_iter_as_int64(&imodel);
        }
        else
            ast_log(LOG_WARNING, "\"%s\" is not found in %s\n", model_name, property);
    } while(0);
    ast_mutex_unlock(&model_lock);
    return btype;
}

/*!
 * \brief   check if the models library has specified collection.
 * \param   collection  is name of model to be retrieved.
 * \retval  true if the library poses the specified collection,
 * \retval  false if not exist.
 */
static bool model_check(const char* collection) 
{
    bson_iter_t iter;
    bool result;
    ast_mutex_lock(&model_lock);
    do {
        result =  bson_iter_init (&iter, models) && bson_iter_find (&iter, collection);
    } while(0);
    ast_mutex_unlock(&model_lock);
    return result;
}

static void model_register(const char *collection, const bson_t *model) 
{
    ast_mutex_lock(&model_lock);
    do {
        if (model_check(collection))
            ast_log(LOG_DEBUG, "%s already registered\n", collection);
        else if (!BSON_APPEND_DOCUMENT(models, collection, model))
            ast_log(LOG_ERROR, "cannot register %s\n", collection);
        else {
            LOG_BSON_AS_JSON(LOG_DEBUG, "models is \"%s\"\n", models);
        }
    } while(0);
    ast_mutex_unlock(&model_lock);
}

static bson_type_t rtype2btype (require_type rtype)
{
    bson_type_t btype;
    switch(rtype) {
        case RQ_INTEGER1:
        case RQ_UINTEGER1:
        case RQ_INTEGER2:
        case RQ_UINTEGER2:
        case RQ_INTEGER3:
        case RQ_UINTEGER3:
        case RQ_INTEGER4:
        case RQ_UINTEGER4:
        case RQ_INTEGER8:
        case RQ_UINTEGER8:
        case RQ_FLOAT:
            btype = BSON_TYPE_DOUBLE;
            break;
        case RQ_DATE:
        case RQ_DATETIME:
        case RQ_CHAR:
            btype = BSON_TYPE_UTF8;
            break;
        default:
            ast_log(LOG_ERROR, "unexpected require type %d\n", rtype);
            btype = BSON_TYPE_UNDEFINED;
    }
    return btype;
}

/*!
 * \brief Execute an SQL query and return ast_variable list
 * \param database  is name of database
 * \param table     is name of collection to find specified records
 * \param ap list containing one or more field/operator/value set.
 *
 * Select database and perform query on table, prepare the sql statement
 * Sub-in the values to the prepared statement and execute it. Return results
 * as a ast_variable list.
 *
 * \retval var on success
 * \retval NULL on failure
 *
 * \see http://api.mongodb.org/c/current/finding-document.html
*/
static struct ast_variable *realtime(const char *database, const char *table, const struct ast_variable *fields)
{
    struct ast_variable *var = NULL;
    mongoc_client_t *dbclient;
    mongoc_collection_t *collection = NULL;
    mongoc_cursor_t *cursor = NULL;
    const bson_t *doc = NULL;
    bson_t *query = NULL;

    if(dbpool == NULL) {
        ast_log(LOG_ERROR, "no connection pool\n");
        return NULL;
    }

    dbclient = mongoc_client_pool_pop(dbpool);
    if(dbclient == NULL) {
        ast_log(LOG_ERROR, "no client allocated\n");
        return NULL;
    }

    do {
        query = make_query(fields, NULL);
        if(query == NULL) {
            ast_log(LOG_ERROR, "cannot make a query to find\n");
            break;
        }
        LOG_BSON_AS_JSON(LOG_DEBUG, "query=%s, database=%s, table=%s\n", query, database, table);

        collection = mongoc_client_get_collection(dbclient, database, table);
        cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 1, 0, query, NULL, NULL);
        if (!cursor) {
            LOG_BSON_AS_JSON(LOG_ERROR, "query failed with query=%s, database=%s, table=%s\n", query, database, table);
            break;
        }
        if (mongoc_cursor_next(cursor, &doc)) {
            bson_iter_t iter;
            const char* key;
            const char* value;
            char work[128];
            struct ast_variable *prev = NULL;

            LOG_BSON_AS_JSON(LOG_DEBUG, "query found %s\n", doc);

            if (!bson_iter_init(&iter, doc)) {
                ast_log(LOG_ERROR, "unexpected bson error!\n");
                break;
            }
            while (bson_iter_next(&iter)) {
                if (BSON_ITER_HOLDS_OID(&iter)) {
                    if (strcmp(bson_iter_key(&iter), SERVERID) == 0) {
                        // SERVERID is hidden property for application                      
                        continue;
                    }
                    const bson_oid_t * oid = bson_iter_oid(&iter);
                    bson_oid_to_string(oid, work);
                    value = work;
                    key = key_mongo2asterisk(bson_iter_key(&iter));
                    // ast_log(LOG_DEBUG, "type=%s, key=%s, value=%s\n", "oid", key, value);
                }
                else if (BSON_ITER_HOLDS_UTF8(&iter)) {
                    uint32_t length;
                    value = bson_iter_utf8(&iter, &length);
                    if (!BSON_UTF8_VALIDATE(value, true)) {
                        ast_log(LOG_WARNING, "unexpected invalid bson found\n");
                        continue;
                    }
                    key = key_mongo2asterisk(bson_iter_key(&iter));
                    // ast_log(LOG_DEBUG, "type=%s, key=%s, value=%s\n", "utf8", key, value);
                }
                else if (BSON_ITER_HOLDS_DOUBLE(&iter)) {
                    double d = bson_iter_double(&iter);
                    sprintf(work, "%.10g", d);
                    value = work;
                    key = key_mongo2asterisk(bson_iter_key(&iter));
                    // ast_log(LOG_DEBUG, "type=%s, key=%s, value=%s\n", "double", key, value);
                }
                else {
                    // see http://api.mongodb.org/libbson/current/bson_iter_type.html
                    ast_log(LOG_WARNING, "unexpected bson type, %x\n", bson_iter_type(&iter));
                    continue;
                }

                if (prev) {
                    prev->next = ast_variable_new(key, value, "");
                    if (prev->next)
                        prev = prev->next;
                }
                else
                    prev = var = ast_variable_new(key, value, "");
            }
        }
    } while(0);

    if (doc)
        bson_destroy((bson_t *)doc);
    if (query)
        bson_destroy((bson_t *)query);
    if (cursor)
        mongoc_cursor_destroy(cursor);
    if (collection)
        mongoc_collection_destroy(collection);
    mongoc_client_pool_push(dbpool, dbclient);
    return var;
}

/*!
 * \brief Execute an Select query and return ast_config list
 * \param database  is name of database
 * \param table     is name of collection to find specified records
 * \param fields    is a list containing one or more field/operator/value set.
 *
 * Select database and preform query on table, prepare the sql statement
 * Sub-in the values to the prepared statement and execute it.
 * Execute this prepared query against MongoDB.
 * Return results as an ast_config variable.
 *
 * \retval var on success
 * \retval NULL on failure
 *
 * \see http://api.mongodb.org/c/current/finding-document.html
*/
static struct ast_config* realtime_multi(const char *database, const char *table, const struct ast_variable *fields)
{
    struct ast_config *cfg = NULL;
    struct ast_category *cat = NULL;
    struct ast_variable *var = NULL;
    mongoc_collection_t *collection = NULL;
    mongoc_cursor_t* cursor = NULL;
    mongoc_client_t* dbclient = NULL;
    const bson_t* doc = NULL;
    const bson_t* query = NULL;
    const char *initfield;
    char *op;

    if(dbpool == NULL) {
        ast_log(LOG_ERROR, "no connection pool\n");
        return NULL;
    }

    dbclient = mongoc_client_pool_pop(dbpool);
    if(dbclient == NULL) {
        ast_log(LOG_ERROR, "no client allocated\n");
        return NULL;
    }
    initfield = ast_strdupa(fields->name);
    if ((op = strchr(initfield, ' '))) {
        *op = '\0';
    }
    do {
        query = make_query(fields, initfield);
        if(query == NULL) {
            ast_log(LOG_ERROR, "cannot make a query to find\n");
            break;
        }

        cfg = ast_config_new();
        if (!cfg) {
            ast_log(LOG_WARNING, "out of memory!\n");
            break;
        }

        collection = mongoc_client_get_collection(dbclient, database, table);

        LOG_BSON_AS_JSON(LOG_DEBUG, "query=%s, database=%s, table=%s\n", query, database, table);

        cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 0, 0, query, NULL, NULL);
        if (!cursor) {
            LOG_BSON_AS_JSON(LOG_ERROR, "query failed with query=%s, database=%s, table=%s\n", query, database, table);
            break;
        }

        while (mongoc_cursor_next(cursor, &doc)) {
            bson_iter_t iter;
            const char* key;
            const char* value;
            char work[128];

            LOG_BSON_AS_JSON(LOG_DEBUG, "query found %s\n", doc);

            if (!bson_iter_init(&iter, doc)) {
                ast_log(LOG_ERROR, "unexpected bson error!\n");
                break;
            }
            cat = ast_category_new("", "", 99999);
            if (!cat) {
                ast_log(LOG_WARNING, "out of memory!\n");
                break;
            }
            while (bson_iter_next(&iter)) {
                if (BSON_ITER_HOLDS_OID(&iter)) {
                    if (strcmp(bson_iter_key(&iter), SERVERID) == 0) {
                        // SERVERID is hidden property for application                      
                        continue;
                    }
                    const bson_oid_t * oid = bson_iter_oid(&iter);
                    bson_oid_to_string(oid, work);
                    value = work;
                    key = key_mongo2asterisk(bson_iter_key(&iter));
                    // ast_log(LOG_DEBUG, "type=%s, key=%s, value=%s\n", "oid", key, value);
                }
                else if (BSON_ITER_HOLDS_UTF8(&iter)) {
                    uint32_t length;
                    value = bson_iter_utf8(&iter, &length);
                    if (!BSON_UTF8_VALIDATE(value, true)) {
                        ast_log(LOG_WARNING, "unexpected invalid bson found\n");
                        continue;
                    }
                    key = key_mongo2asterisk(bson_iter_key(&iter));
                    // ast_log(LOG_DEBUG, "type=%s, key=%s, value=%s\n", "utf8", key, value);
                }
                else if (BSON_ITER_HOLDS_DOUBLE(&iter)) {
                    double d = bson_iter_double(&iter);
                    sprintf(work, "%.10g", d);
                    value = work;
                    key = key_mongo2asterisk(bson_iter_key(&iter));
                    // ast_log(LOG_DEBUG, "type=%s, key=%s, value=%s\n", "double", key, value);
                }
                else {
                    // see http://api.mongodb.org/libbson/current/bson_iter_type.html
                    ast_log(LOG_WARNING, "unexpected bson type, %x\n", bson_iter_type(&iter));
                    continue;
                }

                if (!strcmp(initfield, key)) {
                    ast_category_rename(cat, value);
                }
                var = ast_variable_new(key, value, "");
                ast_variable_append(cat, var);
            }
            ast_category_append(cfg, cat);
        }
    } while(0);
    ast_log(LOG_DEBUG, "end of query.\n");

    if (query)
        bson_destroy((bson_t *)query);
    if (cursor)
        mongoc_cursor_destroy(cursor);
    if (collection)
        mongoc_collection_destroy(collection);
    mongoc_client_pool_push(dbpool, dbclient);
    return cfg;
}

/*!
 * \brief Execute an UPDATE query
 * \param database  is name of database
 * \param table     is name of collection to update records
 * \param keyfield  where clause field
 * \param lookup    value of field for where clause
 * \param fields    is a list containing one or more field/value set(s).
 *
 * Update a database table, prepare the sql statement using keyfield and lookup
 * control the number of records to change. All values to be changed are stored in ap list.
 * Sub-in the values to the prepared statement and execute it.
 *
 * \retval number of rows affected
 * \retval -1 on failure
*/
static int update(const char *database, const char *table, const char *keyfield, const char *lookup, const struct ast_variable *fields)
{
    int ret = -1;
    bson_t *query = NULL;
    bson_t *data = NULL;
    bson_t *update = NULL;
    mongoc_client_t *dbclient = NULL;
    mongoc_collection_t *collection = NULL;

    if (!table || !fields || !keyfield || !lookup) {
        ast_log(LOG_ERROR, "not enough arguments\n");
        return -1;
    }
    if (!model_check(table)) {
        ast_log(LOG_ERROR, "no reference model for %s\n", table);
        return -1;
    }
    if (!fields) {
        ast_log(LOG_NOTICE, "no fields to update\n");
        return 0;
    }
    if(dbpool == NULL) {
        ast_log(LOG_ERROR, "no connection pool\n");
        return -1;
    }
    dbclient = mongoc_client_pool_pop(dbpool);
    if(dbclient == NULL) {
        ast_log(LOG_ERROR, "no client allocated\n");
        return -1;
    }

    do {
        bson_error_t error;
        bool err;

        data = bson_new();
        if (!data) {
            ast_log(LOG_ERROR, "not enough memory\n");
            break;
        }
        update = bson_new();
        if (!update) {
            ast_log(LOG_ERROR, "not enough memory\n");
            break;
        }
        query = serverid ? BCON_NEW(SERVERID, BCON_OID(serverid)) : bson_new();
        if (!query) {
            ast_log(LOG_ERROR, "not enough memory\n");
            break;
        }
        if (!BSON_APPEND_UTF8(query, keyfield, lookup)) {
            ast_log(LOG_ERROR, "cannot make a query\n");
            break;
        }

        collection = mongoc_client_get_collection(dbclient, database, table);

        for (err = false; fields && !err; fields = fields->next) {
            if (strlen(fields->value) == 0)
                continue;
            bson_type_t btype = model_get_btype(table, fields->name);
            switch(btype) {
                case BSON_TYPE_UTF8:
                    err = !BSON_APPEND_UTF8(data, fields->name, fields->value);
                    break;
                case BSON_TYPE_DOUBLE:
                    err = !BSON_APPEND_DOUBLE(data, fields->name, (double)atof(fields->value));
                    break;
                default:
                    ast_log(LOG_WARNING, 
                        "not supported btype=%d. database=%s, table=%s, keyfield=%s, lookup=%s\n", 
                        btype, database, table, keyfield, lookup);
            }
        }
        if (err) {
            ast_log(LOG_ERROR, "cannot make data to update\n");
            break;
        }

        if (!BSON_APPEND_DOCUMENT(update, "$set", data)) {
            ast_log(LOG_ERROR, "cannot make data to update, database=%s, table=%s, keyfield=%s, lookup=%s\n",
                    database, table, keyfield, lookup);
            break;
        }

        LOG_BSON_AS_JSON(LOG_DEBUG, "query=%s\n", query);
        LOG_BSON_AS_JSON(LOG_DEBUG, "update=%s\n", update);

        if (!mongoc_collection_update(collection, MONGOC_UPDATE_NONE, query, update, NULL, &error)) {
            ast_log(LOG_ERROR, "update failed, error=%s\n", error.message);
            LOG_BSON_AS_JSON(LOG_ERROR, "query=%s\n", query);
            LOG_BSON_AS_JSON(LOG_ERROR, "update=%s\n", update);
            break;
        }

        ret = 0; // success
    } while(0);

    if (data)
        bson_destroy((bson_t *)data);
    if (update)
        bson_destroy((bson_t *)update);
    if (query)
        bson_destroy((bson_t *)query);
    if (collection)
        mongoc_collection_destroy(collection);
    mongoc_client_pool_push(dbpool, dbclient);
    return ret;
}

/*!
 * \brief Callback for ast_realtime_require
 * \retval 0 Required fields met specified standards
 * \retval -1 One or more fields was missing or insufficient
 */
static int require(const char *database, const char *table, va_list ap)
{
    bson_t *model = bson_new();
    char *elm;

    while ((elm = va_arg(ap, char *))) {
        int type = va_arg(ap, require_type);
        // int size =
        va_arg(ap, int);
        // ast_log(LOG_DEBUG, "elm=%s, type=%d, size=%d\n", elm, type, size);
        BSON_APPEND_INT64(model, elm, rtype2btype(type));
    }
    LOG_BSON_AS_JSON(LOG_DEBUG, "required model is \"%s\"\n", model);

    model_register(table, model);
    bson_destroy(model);
    return 0;
}

/*!
 * \brief Execute an UPDATE query
 * \param database  is name of database
 * \param table     is name of collection to update records
 * \param ap list containing one or more field/value set(s).
 *
 * Update a database table, preparing the sql statement from a list of
 * key/value pairs specified in ap.  The lookup pairs are specified first
 * and are separated from the update pairs by a sentinel value.
 * Sub-in the values to the prepared statement and execute it.
 *
 * \retval number of rows affected
 * \retval -1 on failure
*/
static int update2(const char *database, const char *table, const struct ast_variable *lookup_fields, const struct ast_variable *update_fields)
{
    ast_log(LOG_DEBUG, "database=%s, table=%s\n", database, table);
    //todo
    return -1;
}

/*!
 * \brief Execute an INSERT query
 * \param database
 * \param table
 * \param ap list containing one or more field/value set(s)
 *
 * Insert a new record into database table, prepare the sql statement.
 * All values to be changed are stored in ap list.
 * Sub-in the values to the prepared statement and execute it.
 *
 * \retval number of rows affected
 * \retval -1 on failure
*/
static int store(const char *database, const char *table, const struct ast_variable *fields)
{
    ast_log(LOG_DEBUG, "database=%s, table=%s\n", database, table);
    //todo
    return -1;
}

/*!
 * \brief Execute an DELETE query
 * \param database
 * \param table
 * \param keyfield where clause field
 * \param lookup value of field for where clause
 * \param ap list containing one or more field/value set(s)
 *
 * Delete a row from a database table, prepare the sql statement using keyfield and lookup
 * control the number of records to change. Additional params to match rows are stored in ap list.
 * Sub-in the values to the prepared statement and execute it.
 *
 * \retval number of rows affected
 * \retval -1 on failure
*/
static int destroy(const char *database, const char *table, const char *keyfield, const char *lookup, const struct ast_variable *fields)
{
    ast_log(LOG_DEBUG, "database=%s, table=%s\n", database, table);
    //todo
    return -1;
}

static struct ast_config *load(
    const char *database, const char *table, const char *file, struct ast_config *cfg, struct ast_flags flags, const char *sugg_incl, const char *who_asked)
{
    struct ast_variable *new_v;
    struct ast_category *cur_cat;
    struct ast_flags loader_flags = { 0 };
    mongoc_collection_t *collection = NULL;
    mongoc_cursor_t* cursor = NULL;
    mongoc_client_t* dbclient = NULL;
    bson_t *query = NULL;
    const bson_t *doc = NULL;
    const bson_t *order = NULL;
    const bson_t *root = NULL;
    const bson_t *fields = NULL;
    const char *last_category = "";
    int last_cat_metric = -1;

    if (!database || !table || !file || !cfg || !who_asked) {
        ast_log(LOG_ERROR, "not enough arguments\n");
        return NULL;
    }
    if (!strcmp (file, CONFIG_FILE))
        return NULL;        /* cant configure myself with myself ! */
    if(dbpool == NULL) {
        ast_log(LOG_ERROR, "no connection pool\n");
        return NULL;
    }

    dbclient = mongoc_client_pool_pop(dbpool);
    if(dbclient == NULL) {
        ast_log(LOG_ERROR, "no client allocated\n");
        return NULL;
    }

    do {
        query = serverid ? BCON_NEW(SERVERID, BCON_OID(serverid)) : bson_new();
        if (!BSON_APPEND_UTF8(query, "filename", file)) {
            ast_log(LOG_ERROR, "unexpected bson error with filename=%s\n", file);
            break;
        }
        if (!BSON_APPEND_DOUBLE(query, "commented", 0)) {
            ast_log(LOG_ERROR, "unexpected bson error\n");
            break;
        }
        order = BCON_NEW(   "cat_metric", BCON_DOUBLE(-1),
                            "var_metric", BCON_DOUBLE(1),
                            "category", BCON_DOUBLE(1),
                            "var_name", BCON_DOUBLE(1));
        root = BCON_NEW(    "$query", BCON_DOCUMENT(query),
                            "$orderby", BCON_DOCUMENT(order));
        fields = BCON_NEW(  "cat_metric", BCON_DOUBLE(1),
                            "category", BCON_DOUBLE(1),
                            "var_name", BCON_DOUBLE(1),
                            "var_val", BCON_DOUBLE(1));

        LOG_BSON_AS_JSON(LOG_DEBUG, "query=%s\n", root);
        // LOG_BSON_AS_JSON(LOG_DEBUG, "fields=%s\n", fields);

        collection = mongoc_client_get_collection(dbclient, database, table);
        cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 0, 0, root, fields, NULL);
        if (!cursor) {
            LOG_BSON_AS_JSON(LOG_ERROR, "query failed with query=%s\n", root);
            LOG_BSON_AS_JSON(LOG_ERROR, "query failed with fields=%s\n", fields);
            break;
        }

        cur_cat = ast_config_get_current_category(cfg);

        while (mongoc_cursor_next(cursor, &doc)) {
            bson_iter_t iter;
            const char *var_name;
            const char *var_val;
            const char *category;
            int cat_metric;
            uint32_t length;

            LOG_BSON_AS_JSON(LOG_DEBUG, "query found %s\n", doc);

            if (!bson_iter_init(&iter, doc)) {
                ast_log(LOG_ERROR, "unexpected bson error!\n");
                break;
            }

            if(!bson_iter_find(&iter, "cat_metric")) {
                ast_log(LOG_ERROR, "no cat_metric found!\n");
                break;
            }
            cat_metric = (int)bson_iter_double(&iter);

            if(!bson_iter_find(&iter, "category")) {
                ast_log(LOG_ERROR, "no category found!\n");
                break;
            }
            category = bson_iter_utf8(&iter, &length);
            if (!category) {
                ast_log(LOG_ERROR, "cannot read category.\n");
                break;
            }

            if(!bson_iter_find(&iter, "var_name")) {
                ast_log(LOG_ERROR, "no var_name found!\n");
                break;
            }
            var_name = bson_iter_utf8(&iter, &length);
            if (!var_name) {
                ast_log(LOG_ERROR, "cannot read var_name.\n");
                break;
            }

            if(!bson_iter_find(&iter, "var_val")) {
                ast_log(LOG_ERROR, "no var_val found!\n");
                break;
            }
            var_val = bson_iter_utf8(&iter, &length);
            if (!var_val) {
                ast_log(LOG_ERROR, "cannot read var_val.\n");
                break;
            }

            if (!strcmp (var_val, "#include")) {
                if (!ast_config_internal_load(var_val, cfg, loader_flags, "", who_asked)) {
                    ast_log(LOG_DEBUG, "ended with who_asked=%s\n", who_asked);
                    break;
                }
                ast_log(LOG_DEBUG, "#include ignored, who_asked=%s\n", who_asked);
                continue;
            }

            if (strcmp(last_category, category) || last_cat_metric != cat_metric) {
                cur_cat = ast_category_new(category, "", 99999);
                if (!cur_cat) {
                    ast_log(LOG_WARNING, "Out of memory!\n");
                    break;
                }
                last_category = category;
                last_cat_metric = cat_metric;
                ast_category_append(cfg, cur_cat);
            }

            new_v = ast_variable_new(var_name, var_val, "");
            ast_variable_append(cur_cat, new_v);
        }
    } while(0);

    if (doc)
        bson_destroy((bson_t *)doc);
    if (fields)
        bson_destroy((bson_t *)fields);
    if (query)
        bson_destroy((bson_t *)query);
    if (order)
        bson_destroy((bson_t *)order);
    if (root)
        bson_destroy((bson_t *)root);
    if (cursor)
        mongoc_cursor_destroy(cursor);
    if (collection)
        mongoc_collection_destroy(collection);
    mongoc_client_pool_push(dbpool, dbclient);
    return cfg;
}

/*!
 * \brief Callback for clearing any cached info
 * \note We don't currently cache anything
 * \retval 0 If any cache was purged
 * \retval -1 If no cache was found
 */
static int unload(const char *a, const char *b)
{
    ast_log(LOG_DEBUG, "database=%s, table=%s\n", a, b);
    /* We currently do no caching */
    return -1;
}


static int config(int reload)
{
    int res = -1;
    struct ast_config *cfg = NULL;
    mongoc_uri_t *uri = NULL;
    ast_log(LOG_DEBUG, "reload=%d\n", reload);

    do {
        const char *tmp;
        struct ast_variable *var;
        struct ast_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

        cfg = ast_config_load(CONFIG_FILE, config_flags);
        if (!cfg || cfg == CONFIG_STATUS_FILEINVALID) {
            ast_log(LOG_WARNING, "unable to load %s\n", CONFIG_FILE);
            res = AST_MODULE_LOAD_DECLINE;
            break;
        } else if (cfg == CONFIG_STATUS_FILEUNCHANGED)
            break;

        var = ast_variable_browse(cfg, CATEGORY);
        if (!var) {
            ast_log(LOG_WARNING, "no category %s specified.\n", CATEGORY);
            break;
        }

        if ((tmp = ast_variable_retrieve(cfg, CATEGORY, "uri")) == NULL) {
            ast_log(LOG_WARNING, "no uri specified.\n");
            break;
        }
        uri = mongoc_uri_new(tmp);
        if (uri == NULL) {
            ast_log(LOG_ERROR, "parsing uri error, %s\n", tmp);
            break;
        }
        if (dbpool)
            mongoc_client_pool_destroy(dbpool);
        dbpool = mongoc_client_pool_new(uri);
        if (dbpool == NULL) {
            ast_log(LOG_ERROR, "cannot make a connection pool for MongoDB\n");
            break;
        }

        if ((tmp = ast_variable_retrieve(cfg, CATEGORY, SERVERID)) != NULL) {
            if (!bson_oid_is_valid (tmp, strlen(tmp))) {
                ast_log(LOG_ERROR, "invalid server id specified.\n");
                break;
            }
            serverid = ast_malloc(sizeof(bson_oid_t));
            if (serverid == NULL) {
                ast_log(LOG_ERROR, "not enough memory\n");
                break;
            }
            bson_oid_init_from_string(serverid, tmp);
        }

        res = 0; // success
    } while (0);

    if (uri)
       mongoc_uri_destroy(uri);
    if (cfg && cfg != CONFIG_STATUS_FILEUNCHANGED && cfg != CONFIG_STATUS_FILEINVALID) {
        ast_config_destroy(cfg);
    }

    models = bson_new();

    return res;
}

static struct ast_config_engine mongodb_engine = {
    .name = (char *)NAME,
    .load_func = load,
    .realtime_func = realtime,
    .realtime_multi_func = realtime_multi,
    .store_func = store,
    .destroy_func = destroy,
    .update_func = update,
    .update2_func = update2,
    .require_func = require,
    .unload_func = unload,
};

static int unload_module(void)
{
    ast_config_engine_deregister(&mongodb_engine);
    if (models)
       bson_destroy(models);
    return 0;
}

static int load_module(void)
{
    if (config(0))
        return AST_MODULE_LOAD_DECLINE;
    ast_config_engine_register(&mongodb_engine);
    return 0;
}

static int reload_module(void)
{
    return config(1);
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "Realtime MongoDB configuration",
    .support_level = AST_MODULE_SUPPORT_CORE,
    .load = load_module,
    .unload = unload_module,
    .reload = reload_module,
    .load_pri = AST_MODPRI_REALTIME_DRIVER,
);
