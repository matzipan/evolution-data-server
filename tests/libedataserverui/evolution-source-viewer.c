/*
 * evolution-source-viewer.c
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the program; if not, see <http://www.gnu.org/licenses/>
 *
 */

#include <config.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>

#include <libedataserver/libedataserver.h>

/* XXX Even though this is all one file, I'm still being pedantic about data
 *     encapsulation (except for a private struct, even I'm not that anal!).
 *     I expect this program will eventually be too complex for one file
 *     and we'll want to split off an e-source-viewer.[ch]. */

/* Standard GObject macros */
#define E_TYPE_SOURCE_VIEWER \
	(e_source_viewer_get_type ())
#define E_SOURCE_VIEWER(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_SOURCE_VIEWER, ESourceViewer))
#define E_SOURCE_VIEWER_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_SOURCE_VIEWER, ESourceViewerClass))
#define E_IS_SOURCE_VIEWER(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_SOURCE_VIEWER))
#define E_IS_SOURCE_VIEWER_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_SOURCE_VIEWER))
#define E_SOURCE_VIEWER_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_SOURCE_VIEWER, ESourceViewerClass))

typedef struct _ESourceViewer ESourceViewer;
typedef struct _ESourceViewerClass ESourceViewerClass;

struct _ESourceViewer {
	GtkWindow parent;
	ESourceRegistry *registry;

	GtkTreeStore *tree_store;
	GHashTable *source_index;

	GtkWidget *tree_view;		/* not referenced */
	GtkWidget *text_view;		/* not referenced */
};

struct _ESourceViewerClass {
	GtkWindowClass parent_class;
};

enum {
	PROP_0,
	PROP_REGISTRY
};

enum {
	COLUMN_DISPLAY_NAME,
	COLUMN_SOURCE_UID,
	COLUMN_REMOVABLE,
	COLUMN_WRITABLE,
	COLUMN_SOURCE,
	NUM_COLUMNS
};

/* Forward Declarations */
GType		e_source_viewer_get_type	(void) G_GNUC_CONST;
GtkWidget *	e_source_viewer_new		(GCancellable *cancellable,
						 GError **error);
ESourceRegistry *
		e_source_viewer_get_registry	(ESourceViewer *viewer);
ESource *	e_source_viewer_ref_selected	(ESourceViewer *viewer);
void		e_source_viewer_set_selected	(ESourceViewer *viewer,
						 ESource *source);
GNode *		e_source_viewer_build_display_tree
						(ESourceViewer *viewer);

static void	e_source_viewer_initable_init	(GInitableIface *interface);

G_DEFINE_TYPE_WITH_CODE (
	ESourceViewer,
	e_source_viewer,
	GTK_TYPE_WINDOW,
	G_IMPLEMENT_INTERFACE (
		G_TYPE_INITABLE,
		e_source_viewer_initable_init));

static gchar *
source_viewer_get_monospace_font_name (void)
{
	GSettings *settings;
	gchar *font_name;

	settings = g_settings_new ("org.gnome.desktop.interface");
	font_name = g_settings_get_string (settings, "monospace-font-name");
	g_object_unref (settings);

	/* Fallback to a reasonable default. */
	if (font_name == NULL)
		font_name = g_strdup ("Monospace 10");

	return font_name;
}

static void
source_viewer_set_text (ESourceViewer *viewer,
                        ESource *source)
{
	GtkTextView *text_view;
	GtkTextBuffer *buffer;
	GtkTextIter start;
	GtkTextIter end;

	text_view = GTK_TEXT_VIEW (viewer->text_view);
	buffer = gtk_text_view_get_buffer (text_view);

	gtk_text_buffer_get_start_iter (buffer, &start);
	gtk_text_buffer_get_end_iter (buffer, &end);
	gtk_text_buffer_delete (buffer, &start, &end);

	if (source != NULL) {
		gchar *string;
		gsize length;

		gtk_text_buffer_get_start_iter (buffer, &start);

		string = e_source_to_string (source, &length);
		gtk_text_buffer_insert (buffer, &start, string, length);
		g_free (string);
	}
}

static void
source_viewer_update_row (ESourceViewer *viewer,
                          ESource *source)
{
	GHashTable *source_index;
	GtkTreeRowReference *reference;
	GtkTreeModel *model;
	GtkTreePath *path;
	GtkTreeIter iter;
	const gchar *display_name;
	const gchar *source_uid;
	gboolean removable;
	gboolean writable;

	source_index = viewer->source_index;
	reference = g_hash_table_lookup (source_index, source);

	/* We show all sources, so the reference should be valid. */
	g_return_if_fail (gtk_tree_row_reference_valid (reference));

	model = gtk_tree_row_reference_get_model (reference);
	path = gtk_tree_row_reference_get_path (reference);
	gtk_tree_model_get_iter (model, &iter, path);
	gtk_tree_path_free (path);

	source_uid = e_source_get_uid (source);
	display_name = e_source_get_display_name (source);
	removable = e_source_get_removable (source);
	writable = e_source_get_writable (source);

	gtk_tree_store_set (
		GTK_TREE_STORE (model), &iter,
		COLUMN_DISPLAY_NAME, display_name,
		COLUMN_SOURCE_UID, source_uid,
		COLUMN_REMOVABLE, removable,
		COLUMN_WRITABLE, writable,
		COLUMN_SOURCE, source,
		-1);
}

static gboolean
source_viewer_traverse (GNode *node,
                        gpointer user_data)
{
	ESourceViewer *viewer;
	ESource *source;
	GHashTable *source_index;
	GtkTreeRowReference *reference = NULL;
	GtkTreeView *tree_view;
	GtkTreeModel *model;
	GtkTreePath *path;
	GtkTreeIter iter;

	/* Skip the root node. */
	if (G_NODE_IS_ROOT (node))
		return FALSE;

	viewer = E_SOURCE_VIEWER (user_data);

	source_index = viewer->source_index;

	tree_view = GTK_TREE_VIEW (viewer->tree_view);
	model = gtk_tree_view_get_model (tree_view);

	if (node->parent != NULL && node->parent->data != NULL)
		reference = g_hash_table_lookup (
			source_index, node->parent->data);

	if (gtk_tree_row_reference_valid (reference)) {
		GtkTreeIter parent;

		path = gtk_tree_row_reference_get_path (reference);
		gtk_tree_model_get_iter (model, &parent, path);
		gtk_tree_path_free (path);

		gtk_tree_store_append (GTK_TREE_STORE (model), &iter, &parent);
	} else
		gtk_tree_store_append (GTK_TREE_STORE (model), &iter, NULL);

	/* Source index takes ownership. */
	source = g_object_ref (node->data);

	path = gtk_tree_model_get_path (model, &iter);
	reference = gtk_tree_row_reference_new (model, path);
	g_hash_table_insert (source_index, source, reference);
	gtk_tree_path_free (path);

	source_viewer_update_row (viewer, source);

	return FALSE;
}

static void
source_viewer_save_expanded (GtkTreeView *tree_view,
                             GtkTreePath *path,
                             GQueue *queue)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	ESource *source;

	model = gtk_tree_view_get_model (tree_view);
	gtk_tree_model_get_iter (model, &iter, path);
	gtk_tree_model_get (model, &iter, COLUMN_SOURCE, &source, -1);
	g_queue_push_tail (queue, source);
}

static void
source_viewer_build_model (ESourceViewer *viewer)
{
	GQueue queue = G_QUEUE_INIT;
	GHashTable *source_index;
	GtkTreeView *tree_view;
	GtkTreeModel *model;
	ESource *selected;
	GNode *root;

	tree_view = GTK_TREE_VIEW (viewer->tree_view);

	source_index = viewer->source_index;
	selected = e_source_viewer_ref_selected (viewer);

	/* Save expanded sources to restore later. */
	gtk_tree_view_map_expanded_rows (
		tree_view, (GtkTreeViewMappingFunc)
		source_viewer_save_expanded, &queue);

	model = gtk_tree_view_get_model (tree_view);
	gtk_tree_store_clear (GTK_TREE_STORE (model));

	g_hash_table_remove_all (source_index);

	root = e_source_viewer_build_display_tree (viewer);

	g_node_traverse (
		root, G_PRE_ORDER, G_TRAVERSE_ALL, -1,
		(GNodeTraverseFunc) source_viewer_traverse, viewer);

	e_source_registry_free_display_tree (root);

	/* Restore previously expanded sources. */
	while (!g_queue_is_empty (&queue)) {
		GtkTreeRowReference *reference;
		ESource *source;

		source = g_queue_pop_head (&queue);
		reference = g_hash_table_lookup (source_index, source);

		if (gtk_tree_row_reference_valid (reference)) {
			GtkTreePath *path;

			path = gtk_tree_row_reference_get_path (reference);
			gtk_tree_view_expand_to_path (tree_view, path);
			gtk_tree_path_free (path);
		}

		g_object_unref (source);
	}

	/* Restore the selected source. */
	if (selected != NULL) {
		e_source_viewer_set_selected (viewer, selected);
		g_object_unref (selected);
	}
}

static void
source_viewer_expand_to_source (ESourceViewer *viewer,
                                ESource *source)
{
	GHashTable *source_index;
	GtkTreeRowReference *reference;
	GtkTreeView *tree_view;
	GtkTreePath *path;

	source_index = viewer->source_index;
	reference = g_hash_table_lookup (source_index, source);

	/* We show all sources, so the reference should be valid. */
	g_return_if_fail (gtk_tree_row_reference_valid (reference));

	/* Expand the tree view to the path containing the ESource. */
	tree_view = GTK_TREE_VIEW (viewer->tree_view);
	path = gtk_tree_row_reference_get_path (reference);
	gtk_tree_view_expand_to_path (tree_view, path);
	gtk_tree_path_free (path);
}

static void
source_viewer_source_added_cb (ESourceRegistry *registry,
                               ESource *source,
                               ESourceViewer *viewer)
{
	source_viewer_build_model (viewer);

	source_viewer_expand_to_source (viewer, source);
}

static void
source_viewer_source_changed_cb (ESourceRegistry *registry,
                                 ESource *source,
                                 ESourceViewer *viewer)
{
	ESource *selected;

	source_viewer_update_row (viewer, source);

	selected = e_source_viewer_ref_selected (viewer);
	if (selected != NULL) {
		if (e_source_equal (source, selected))
			source_viewer_set_text (viewer, source);
		g_object_unref (selected);
	}
}

static void
source_viewer_source_removed_cb (ESourceRegistry *registry,
                                 ESource *source,
                                 ESourceViewer *viewer)
{
	source_viewer_build_model (viewer);
}

static void
source_viewer_selection_changed_cb (GtkTreeSelection *selection,
                                    ESourceViewer *viewer)
{
	ESource *source;

	source = e_source_viewer_ref_selected (viewer);

	source_viewer_set_text (viewer, source);

	if (source != NULL)
		g_object_unref (source);
}

static void
source_viewer_get_property (GObject *object,
                            guint property_id,
                            GValue *value,
                            GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_REGISTRY:
			g_value_set_object (
				value,
				e_source_viewer_get_registry (
				E_SOURCE_VIEWER (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
source_viewer_dispose (GObject *object)
{
	ESourceViewer *viewer = E_SOURCE_VIEWER (object);

	if (viewer->registry != NULL) {
		g_signal_handlers_disconnect_matched (
			viewer->registry,
			G_SIGNAL_MATCH_DATA,
			0, 0, NULL, NULL, object);
		g_object_unref (viewer->registry);
		viewer->registry = NULL;
	}

	if (viewer->tree_store != NULL) {
		g_object_unref (viewer->tree_store);
		viewer->tree_store = NULL;
	}

	g_hash_table_remove_all (viewer->source_index);

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_source_viewer_parent_class)->dispose (object);
}

static void
source_viewer_finalize (GObject *object)
{
	ESourceViewer *viewer = E_SOURCE_VIEWER (object);

	g_hash_table_destroy (viewer->source_index);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (e_source_viewer_parent_class)->finalize (object);
}

static void
source_viewer_constructed (GObject *object)
{
	ESourceViewer *viewer;
	GtkTreeViewColumn *column;
	GtkTreeSelection *selection;
	GtkCellRenderer *renderer;
	GtkWidget *container;
	GtkWidget *paned;
	GtkWidget *widget;
	PangoFontDescription *desc;
	const gchar *title;
	gchar *font_name;

	viewer = E_SOURCE_VIEWER (object);

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (e_source_viewer_parent_class)->constructed (object);

	title = _("Evolution Source Viewer");
	gtk_window_set_title (GTK_WINDOW (viewer), title);
	gtk_window_set_default_size (GTK_WINDOW (viewer), 800, 600);

	paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_set_position (GTK_PANED (paned), 400);
	gtk_container_add (GTK_CONTAINER (viewer), paned);
	gtk_widget_show (paned);

	/* Left panel */

	widget = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (
		GTK_SCROLLED_WINDOW (widget),
		GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (
		GTK_SCROLLED_WINDOW (widget), GTK_SHADOW_IN);
	gtk_paned_add1 (GTK_PANED (paned), widget);
	gtk_widget_show (widget);

	container = widget;

	widget = gtk_tree_view_new_with_model (
		GTK_TREE_MODEL (viewer->tree_store));
	gtk_container_add (GTK_CONTAINER (container), widget);
	viewer->tree_view = widget;  /* do not reference */
	gtk_widget_show (widget);

	column = gtk_tree_view_column_new ();
	gtk_tree_view_column_set_title (column, _("Display Name"));
	gtk_tree_view_append_column (GTK_TREE_VIEW (widget), column);

	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start (column, renderer, TRUE);
	gtk_tree_view_column_add_attribute (
		column, renderer, "text", COLUMN_DISPLAY_NAME);

	column = gtk_tree_view_column_new ();
	gtk_tree_view_column_set_title (column, _("Flags"));
	gtk_tree_view_append_column (GTK_TREE_VIEW (widget), column);

	renderer = gtk_cell_renderer_pixbuf_new ();
	g_object_set (
		renderer,
		"stock-id", GTK_STOCK_EDIT,
		"stock-size", GTK_ICON_SIZE_MENU,
		NULL);
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_add_attribute (
		column, renderer, "visible", COLUMN_WRITABLE);

	renderer = gtk_cell_renderer_pixbuf_new ();
	g_object_set (
		renderer,
		"stock-id", GTK_STOCK_DELETE,
		"stock-size", GTK_ICON_SIZE_MENU,
		NULL);
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_add_attribute (
		column, renderer, "visible", COLUMN_REMOVABLE);

	column = gtk_tree_view_column_new ();
	gtk_tree_view_column_set_title (column, _("Identity"));
	gtk_tree_view_append_column (GTK_TREE_VIEW (widget), column);

	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_add_attribute (
		column, renderer, "text", COLUMN_SOURCE_UID);

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));

	/* Right panel */

	widget = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (
		GTK_SCROLLED_WINDOW (widget),
		GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (
		GTK_SCROLLED_WINDOW (widget), GTK_SHADOW_IN);
	gtk_paned_add2 (GTK_PANED (paned), widget);
	gtk_widget_show (widget);

	container = widget;

	widget = gtk_text_view_new ();
	gtk_text_view_set_editable (GTK_TEXT_VIEW (widget), FALSE);
	gtk_container_add (GTK_CONTAINER (container), widget);
	viewer->text_view = widget;  /* do not reference */
	gtk_widget_show (widget);

	font_name = source_viewer_get_monospace_font_name ();
	desc = pango_font_description_from_string (font_name);
	gtk_widget_override_font (widget, desc);
	pango_font_description_free (desc);
	g_free (font_name);

	g_signal_connect (
		selection, "changed",
		G_CALLBACK (source_viewer_selection_changed_cb), viewer);
}

static gboolean
source_viewer_initable_init (GInitable *initable,
                             GCancellable *cancellable,
                             GError **error)
{
	ESourceViewer *viewer;
	ESourceRegistry *registry;

	viewer = E_SOURCE_VIEWER (initable);

	registry = e_source_registry_new_sync (cancellable, error);

	if (registry == NULL)
		return FALSE;

	viewer->registry = registry;  /* takes ownership */

	g_signal_connect (
		registry, "source-added",
		G_CALLBACK (source_viewer_source_added_cb), viewer);

	g_signal_connect (
		registry, "source-changed",
		G_CALLBACK (source_viewer_source_changed_cb), viewer);

	g_signal_connect (
		registry, "source-removed",
		G_CALLBACK (source_viewer_source_removed_cb), viewer);

	source_viewer_build_model (viewer);

	gtk_tree_view_expand_all (GTK_TREE_VIEW (viewer->tree_view));

	return TRUE;
}

static void
e_source_viewer_class_init (ESourceViewerClass *class)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS (class);
	object_class->get_property = source_viewer_get_property;
	object_class->dispose = source_viewer_dispose;
	object_class->finalize = source_viewer_finalize;
	object_class->constructed = source_viewer_constructed;

	g_object_class_install_property (
		object_class,
		PROP_REGISTRY,
		g_param_spec_object (
			"registry",
			"Registry",
			"Data source registry",
			E_TYPE_SOURCE_REGISTRY,
			G_PARAM_READABLE |
			G_PARAM_STATIC_STRINGS));
}

static void
e_source_viewer_initable_init (GInitableIface *interface)
{
	interface->init = source_viewer_initable_init;
}

static void
e_source_viewer_init (ESourceViewer *viewer)
{
	viewer->tree_store = gtk_tree_store_new (
		NUM_COLUMNS,
		G_TYPE_STRING,		/* COLUMN_DISPLAY_NAME */
		G_TYPE_STRING,		/* COLUMN_SOURCE_UID */
		G_TYPE_BOOLEAN,		/* COLUMN_REMOVABLE */
		G_TYPE_BOOLEAN,		/* COLUMN_WRITABLE */
		E_TYPE_SOURCE);		/* COLUMN_SOURCE */

	viewer->source_index = g_hash_table_new_full (
		(GHashFunc) e_source_hash,
		(GEqualFunc) e_source_equal,
		(GDestroyNotify) g_object_unref,
		(GDestroyNotify) gtk_tree_row_reference_free);
}

GtkWidget *
e_source_viewer_new (GCancellable *cancellable,
                     GError **error)
{
	return g_initable_new (
		E_TYPE_SOURCE_VIEWER,
		cancellable, error, NULL);
}

ESourceRegistry *
e_source_viewer_get_registry (ESourceViewer *viewer)
{
	g_return_val_if_fail (E_IS_SOURCE_VIEWER (viewer), NULL);

	return viewer->registry;
}

ESource *
e_source_viewer_ref_selected (ESourceViewer *viewer)
{
	ESource *source;
	GtkTreeSelection *selection;
	GtkTreeView *tree_view;
	GtkTreeModel *model;
	GtkTreeIter iter;

	g_return_val_if_fail (E_IS_SOURCE_VIEWER (viewer), NULL);

	tree_view = GTK_TREE_VIEW (viewer->tree_view);
	model = gtk_tree_view_get_model (tree_view);
	selection = gtk_tree_view_get_selection (tree_view);

	if (!gtk_tree_selection_get_selected (selection, NULL, &iter))
		return NULL;

	gtk_tree_model_get (model, &iter, COLUMN_SOURCE, &source, -1);

	return source;
}

void
e_source_viewer_set_selected (ESourceViewer *viewer,
                              ESource *source)
{
	GHashTable *source_index;
	GtkTreeRowReference *reference;
	GtkTreeSelection *selection;
	GtkTreeView *tree_view;
	GtkTreePath *path;

	g_return_if_fail (E_IS_SOURCE_VIEWER (viewer));
	g_return_if_fail (E_IS_SOURCE (source));

	tree_view = GTK_TREE_VIEW (viewer->tree_view);
	selection = gtk_tree_view_get_selection (tree_view);

	source_index = viewer->source_index;
	reference = g_hash_table_lookup (source_index, source);

	/* XXX Maybe we should return a success/fail boolean? */
	if (!gtk_tree_row_reference_valid (reference))
		return;

	gtk_tree_selection_unselect_all (selection);

	path = gtk_tree_row_reference_get_path (reference);

	gtk_tree_view_expand_to_path (tree_view, path);
	gtk_tree_selection_select_path (selection, path);

	gtk_tree_path_free (path);
}

/* Helper for e_source_viewer_build_display_tree() */
static gint
source_viewer_compare_nodes (GNode *node_a,
                             GNode *node_b)
{
	ESource *source_a = E_SOURCE (node_a->data);
	ESource *source_b = E_SOURCE (node_b->data);

	return e_source_compare_by_display_name (source_a, source_b);
}

/* Helper for e_source_viewer_build_display_tree() */
static gboolean
source_viewer_sort_nodes (GNode *node,
                          gpointer unused)
{
	GQueue queue = G_QUEUE_INIT;
	GNode *child_node;

	/* Unlink all the child nodes and place them in a queue. */
	while ((child_node = g_node_first_child (node)) != NULL) {
		g_node_unlink (child_node);
		g_queue_push_tail (&queue, child_node);
	}

	/* Sort the queue by source name. */
	g_queue_sort (
		&queue, (GCompareDataFunc)
		source_viewer_compare_nodes, NULL);

	/* Pop nodes off the head of the queue and put them back
	 * under the parent node (preserving the sorted order). */
	while ((child_node = g_queue_pop_head (&queue)) != NULL)
		g_node_append (node, child_node);

	return FALSE;
}

GNode *
e_source_viewer_build_display_tree (ESourceViewer *viewer)
{
	GNode *root;
	GHashTable *index;
	GList *list, *link;
	GHashTableIter iter;
	gpointer value;

	/* This is just like e_source_registry_build_display_tree()
	 * except it includes all data sources, even disabled ones.
	 * Free the tree with e_source_registry_free_display_tree(). */

	g_return_val_if_fail (E_IS_SOURCE_VIEWER (viewer), NULL);

	root = g_node_new (NULL);
	index = g_hash_table_new (g_str_hash, g_str_equal);

	/* Add a GNode for each ESource to the index.
	 * The GNodes take ownership of the ESource references. */
	list = e_source_registry_list_sources (viewer->registry, NULL);
	for (link = list; link != NULL; link = g_list_next (link)) {
		ESource *source = E_SOURCE (link->data);
		gpointer key = (gpointer) e_source_get_uid (source);
		g_hash_table_insert (index, key, g_node_new (source));
	}
	g_list_free (list);

	/* Traverse the index and link the nodes together. */
	g_hash_table_iter_init (&iter, index);
	while (g_hash_table_iter_next (&iter, NULL, &value)) {
		ESource *source;
		GNode *source_node;
		GNode *parent_node;
		const gchar *parent_uid;

		source_node = (GNode *) value;
		source = E_SOURCE (source_node->data);
		parent_uid = e_source_get_parent (source);

		if (parent_uid == NULL || *parent_uid == '\0') {
			parent_node = root;
		} else {
			parent_node = g_hash_table_lookup (index, parent_uid);
			g_warn_if_fail (parent_node != NULL);
		}

		/* Should never be NULL, but just to be safe. */
		if (parent_node != NULL)
			g_node_append (parent_node, source_node);
	}

	/* Sort nodes by display name in post order. */
	g_node_traverse (
		root, G_POST_ORDER, G_TRAVERSE_ALL,
		-1, source_viewer_sort_nodes, NULL);

	g_hash_table_destroy (index);

	return root;
}

gint
main (gint argc,
      gchar **argv)
{
	GtkWidget *viewer;
	GError *error = NULL;

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	gtk_init (&argc, &argv);

	viewer = e_source_viewer_new (NULL, &error);

	if (error != NULL) {
		g_warn_if_fail (viewer == NULL);
		g_error ("%s", error->message);
		g_assert_not_reached ();
	}

	g_signal_connect (
		viewer, "delete-event",
		G_CALLBACK (gtk_main_quit), NULL);

	gtk_widget_show (viewer);

	gtk_main ();

	return 0;
}