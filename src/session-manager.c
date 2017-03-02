#include <errno.h>
#include <error.h>
#include <glib.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "session-manager.h"

static gpointer session_manager_parent_class = NULL;

enum {
    SIGNAL_0,
    SIGNAL_NEW_SESSION,
    N_SIGNALS,
};

enum {
    PROP_0,
    PROP_MAX_CONNECTIONS,
    N_PROPERTIES,
};
static GParamSpec *obj_properties [N_PROPERTIES] = { NULL, };

static guint signals [N_SIGNALS] = { 0, };

/*
 * GObject property setter.
 */
static void
session_manager_set_property (GObject        *object,
                              guint           property_id,
                              GValue const   *value,
                              GParamSpec     *pspec)
{
    SessionManager *mgr = SESSION_MANAGER (object);

    g_debug ("session_manager_set_property: 0x%" PRIxPTR,
             (uintptr_t)mgr);
    switch (property_id) {
    case PROP_MAX_CONNECTIONS:
        mgr->max_connections = g_value_get_uint (value);
        g_debug ("  max_connections: 0x%" PRIxPTR,
                 (uintptr_t)mgr->max_connections);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}
/*
 * GObject property getter.
 */
static void
session_manager_get_property (GObject     *object,
                              guint        property_id,
                              GValue      *value,
                              GParamSpec  *pspec)
{
    SessionManager *mgr = SESSION_MANAGER (object);

    g_debug ("session_manager_get_property: 0x%" PRIxPTR, (uintptr_t)mgr);
    switch (property_id) {
    case PROP_MAX_CONNECTIONS:
        g_value_set_uint (value, mgr->max_connections);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

SessionManager*
session_manager_new (guint max_connections)
{
    SessionManager *session_manager;

    session_manager = SESSION_MANAGER (g_object_new (TYPE_SESSION_MANAGER,
                                                     "max-connections", max_connections,
                                                     NULL));
    if (pthread_mutex_init (&session_manager->mutex, NULL) != 0)
        g_error ("Failed to initialize session _manager mutex: %s",
                 strerror (errno));
    /* These two data structures must be kept in sync. When the
     * session-manager object is destoryed the session-data objects in these
     * hash tables will be free'd by the g_object_unref function. We only
     * set this for one of the hash tables because we only want to free
     * each session-data object once.
     */
    session_manager->session_from_fd_table =
        g_hash_table_new_full (g_int_hash,
                               session_data_equal_fd,
                               NULL,
                               NULL);
    session_manager->session_from_id_table =
        g_hash_table_new_full (g_int64_hash,
                               session_data_equal_id,
                               NULL,
                               (GDestroyNotify)g_object_unref);
    return session_manager;
}

static void
session_manager_finalize (GObject *obj)
{
    gint ret;
    SessionManager *manager = SESSION_MANAGER (obj);

    ret = pthread_mutex_lock (&manager->mutex);
    if (ret != 0)
        g_error ("Error locking session_manager mutex: %s",
                 strerror (errno));
    g_hash_table_unref (manager->session_from_fd_table);
    g_hash_table_unref (manager->session_from_id_table);
    ret = pthread_mutex_unlock (&manager->mutex);
    if (ret != 0)
        g_error ("Error unlocking session_manager mutex: %s",
                 strerror (errno));
    ret = pthread_mutex_destroy (&manager->mutex);
    if (ret != 0)
        g_error ("Error destroying session_manager mutex: %s",
                 strerror (errno));
}
/**
 * Boilerplate GObject class init function. The only interesting thing that
 * we do here is creating / registering the 'new-session'signal. This signal
 * invokes callbacks with the new_session_callback type (see header). This
 * signal is emitted by the session_manager_insert function which is where
 * we add new sessions to those tracked by the SessionManager.
 */
static void
session_manager_class_init (gpointer klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    if (session_manager_parent_class == NULL)
        session_manager_parent_class = g_type_class_peek_parent (klass);
    object_class->finalize = session_manager_finalize;
    object_class->get_property = session_manager_get_property;
    object_class->set_property = session_manager_set_property;
    signals [SIGNAL_NEW_SESSION] =
        g_signal_new ("new-session",
                      G_TYPE_FROM_CLASS (object_class),
                      G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS,
                      0,
                      NULL,
                      NULL,
                      NULL,
                      G_TYPE_INT,
                      1,
                      TYPE_SESSION_DATA);
    obj_properties [PROP_MAX_CONNECTIONS] =
        g_param_spec_uint ("max-connections",
                           "max connections",
                           "Maximum nunmber of concurrent client connections",
                           0,
                           MAX_CONNECTIONS,
                           MAX_CONNECTIONS_DEFAULT,
                           G_PARAM_READWRITE);
    g_object_class_install_properties (object_class,
                                       N_PROPERTIES,
                                       obj_properties);
}

GType
session_manager_get_type (void)
{
    static GType type = 0;

    if (type == 0) {
        type = g_type_register_static_simple (G_TYPE_OBJECT,
                                              "SessionManager",
                                              sizeof (SessionManagerClass),
                                              (GClassInitFunc) session_manager_class_init,
                                              sizeof (SessionManager),
                                              NULL,
                                              0);
    }
    return type;
}
gint
session_manager_insert (SessionManager    *manager,
                        SessionData       *session)
{
    gint ret;

    ret = pthread_mutex_lock (&manager->mutex);
    if (ret != 0)
        g_error ("Error locking session_manager mutex: %s",
                 strerror (errno));
    if (session_manager_is_full (manager)) {
        g_warning ("session_manager: 0x%" PRIxPTR " max_connections of %u exceeded",
                   (uintptr_t)manager, manager->max_connections);
        pthread_mutex_unlock (&manager->mutex);
        return -1;
    }
    /*
     * Increase reference count on SessionData object on insert. The
     * corresponding call to g_hash_table_remove will cause the reference
     * count to be decreased (see g_hash_table_new_full).
     */
    g_object_ref (session);
    g_hash_table_insert (manager->session_from_fd_table,
                         session_data_key_fd (session),
                         session);
    g_hash_table_insert (manager->session_from_id_table,
                         session_data_key_id (session),
                         session);
    ret = pthread_mutex_unlock (&manager->mutex);
    if (ret != 0)
        g_error ("Error unlocking session_manager mutex: %s",
                 strerror (errno));
    /* not sure what to do about reference count on SEssionData obj */
    g_signal_emit (manager, signals [SIGNAL_NEW_SESSION], 0, session, &ret);
    return ret;
}
/*
 * Lookup a SessionData object from the provided session fd. This function
 * returns a reference to the SessionData object. The reference count for
 * this object is incremented before it is returned and must be decremented
 * by the caller.
 */
SessionData*
session_manager_lookup_fd (SessionManager *manager,
                           gint fd_in)
{
    SessionData *session;

    pthread_mutex_lock (&manager->mutex);
    session = g_hash_table_lookup (manager->session_from_fd_table,
                                   &fd_in);
    pthread_mutex_unlock (&manager->mutex);
    g_object_ref (session);

    return session;
}
/*
 * Lookup a SessionData object from the provided session ID. This function
 * returns a reference to the SessionData object. The reference count for
 * this object is incremented before it is returned and must be decremented
 * by the caller.
 */
SessionData*
session_manager_lookup_id (SessionManager   *manager,
                           gint64 id)
{
    SessionData *session;

    g_debug ("locking manager mutex");
    pthread_mutex_lock (&manager->mutex);
    g_debug ("g_hash_table_lookup: session_from_id_table");
    session = g_hash_table_lookup (manager->session_from_id_table,
                                   &id);
    g_debug ("unlocking manager mutex");
    pthread_mutex_unlock (&manager->mutex);
    g_object_ref (session);

    return session;
}

gboolean
session_manager_remove (SessionManager    *manager,
                        SessionData       *session)
{
    gboolean ret;

    g_debug ("session_manager 0x%" PRIxPTR " removing session 0x%" PRIxPTR,
             (uintptr_t)manager, (uintptr_t)session);
    pthread_mutex_lock (&manager->mutex);
    ret = g_hash_table_remove (manager->session_from_fd_table,
                               session_data_key_fd (session));
    if (ret != TRUE)
        g_error ("failed to remove session 0x%" PRIxPTR " from g_hash_table "
                 "0x%" PRIxPTR "using key 0x%" PRIxPTR, (uintptr_t)session,
                 (uintptr_t)manager->session_from_fd_table,
                 (uintptr_t)session_data_key_fd (session));
    ret = g_hash_table_remove (manager->session_from_id_table,
                               session_data_key_id (session));
    if (ret != TRUE)
        g_error ("failed to remove session 0x%" PRIxPTR " from g_hash_table "
                 "0x%" PRIxPTR " using key %" PRIxPTR, (uintptr_t)session,
                 (uintptr_t)manager->session_from_fd_table,
                 (uintptr_t)session_data_key_id (session));
    pthread_mutex_unlock (&manager->mutex);

    return ret;
}

void
set_fd (gpointer key,
        gpointer value,
        gpointer user_data)
{
    fd_set *fds = (fd_set*)user_data;
    SessionData *session = SESSION_DATA (value);

    FD_SET (session_data_receive_fd (session), fds);
}

void
session_manager_set_fds (SessionManager   *manager,
                         fd_set *fds)
{
    pthread_mutex_lock (&manager->mutex);
    g_hash_table_foreach (manager->session_from_fd_table,
                          set_fd,
                          fds);
    pthread_mutex_unlock (&manager->mutex);
}

guint
session_manager_size (SessionManager   *manager)
{
    return g_hash_table_size (manager->session_from_fd_table);
}

gboolean
session_manager_is_full (SessionManager *manager)
{
    guint table_size;

    table_size = g_hash_table_size (manager->session_from_fd_table);
    if (table_size < manager->max_connections) {
        return FALSE;
    } else {
        return TRUE;
    }
}
