/*
 *      msgwindow.c - this file is part of Geany, a fast and lightweight IDE
 *
 *      Copyright 2005-2012 Enrico Tröger <enrico(dot)troeger(at)uvena(dot)de>
 *      Copyright 2006-2012 Nick Treleaven <nick(dot)treleaven(at)btinternet(dot)com>
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License along
 *      with this program; if not, write to the Free Software Foundation, Inc.,
 *      51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @file msgwindow.h
 * Message window functions (status, compiler, messages windows).
 * Also compiler error message parsing and grep file and line parsing.
 *
 * @see GeanyMainWidgets::message_window_notebook to append a new notebook page.
 **/

#include "geany.h"

#include "support.h"
#include "prefs.h"
#include "callbacks.h"
#include "ui_utils.h"
#include "utils.h"
#include "document.h"
#include "filetypes.h"
#include "build.h"
#include "main.h"
#include "vte.h"
#include "navqueue.h"
#include "editor.h"
#include "msgwindow.h"
#include "keybindings.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <gdk/gdkkeysyms.h>

#define STORE_MSG_LINE_INDEX 			0
#define STORE_MSG_DOC_INDEX 			1
#define STORE_MSG_DOC_FILENAME_INDEX	2
#define STORE_MSG_MARKUP_INDEX			3

#define MSGWIN_BUTTON_LEFT    1
#define MSGWIN_BUTTON_MIDDLE  2
#define MSGWIN_BUTTON_RIGHT   3

#define MESSAGES_DIR_KEY "messages_dir" 

/* used for parse_file_line */
typedef struct
{
	const gchar *string;	/* line data */
	const gchar *pattern;	/* pattern to split the error message into some fields */
	guint min_fields;		/* used to detect errors after parsing */
	guint line_idx;			/* idx of the field where the line is */
	gint file_idx;			/* idx of the field where the filename is or -1 */
}
ParseData;

MessageWindow msgwindow;


static void prepare_status_tree_view(void);
static void prepare_compiler_tree_view(void);
static GtkWidget *create_message_popup_menu(gint type);
static gboolean on_msgwin_button_press_event(GtkWidget *widget, GdkEventButton *event,
																			gpointer user_data);
static gboolean on_msgwin_key_press_event(GtkWidget *widget, GdkEventKey *event, gpointer data);
static void on_scribble_populate(GtkTextView *textview, GtkMenu *arg1, gpointer user_data);
void msgwin_msg_add_markup_no_validate(gint line, GeanyDocument *doc, const gchar *doc_filename,
												const gchar *markup);


void msgwin_show_hide_tabs(void)
{
	ui_widget_show_hide(gtk_widget_get_parent(msgwindow.tree_status), interface_prefs.msgwin_status_visible);
	ui_widget_show_hide(gtk_widget_get_parent(msgwindow.tree_compiler), interface_prefs.msgwin_compiler_visible);
	ui_widget_show_hide(gtk_widget_get_parent(msgwindow.tree_msg), interface_prefs.msgwin_messages_visible);
	ui_widget_show_hide(gtk_widget_get_parent(msgwindow.scribble), interface_prefs.msgwin_scribble_visible);
}


/** Sets the Messages path for opening any parsed filenames without absolute path
 * from message lines.
 * @param messages_dir The directory. **/
void msgwin_set_messages_dir(const gchar *messages_dir)
{
	g_free(msgwindow.messages_dir);
	msgwindow.messages_dir = g_strdup(messages_dir);
}

static void
notebook_msgwin_tab_close_clicked_cb(GtkButton *button, gpointer user_data)
{
	gint page = gtk_notebook_page_num(GTK_NOTEBOOK(msgwindow.msg_notebook),
		GTK_WIDGET(user_data));

	if (page != -1) {
        gtk_notebook_remove_page(GTK_NOTEBOOK(msgwindow.msg_notebook), page);
    }
}

void msgwin_add_page(const gchar *tab_label, const gchar *messages_dir)
{
	GtkWidget *window;
	GtkWidget *treeview;
	GtkWidget *label;
	GtkListStore *store;
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;
	GtkTreeSelection *selection;
    GtkWidget *hbox;
	GtkWidget *btn;

	window = gtk_widget_new(GTK_TYPE_SCROLLED_WINDOW, "visible", TRUE, 
								"can-focus", TRUE, 
								"hscrollbar-policy", GTK_POLICY_AUTOMATIC,
								"vscrollbar-policy", GTK_POLICY_AUTOMATIC,
								NULL);
	treeview = gtk_widget_new(GTK_TYPE_TREE_VIEW, "visible", TRUE, 
								"can-focus", FALSE,
								"headers-visible", FALSE,
								"rules_hint", TRUE,
								NULL);
	label = gtk_widget_new(GTK_TYPE_LABEL, "visible", TRUE, 
								"can-focus", FALSE,
								"label", tab_label,
								NULL);

	hbox = gtk_hbox_new(FALSE, 2);

	gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

	btn = ui_tab_add_close_button(hbox);
    g_signal_connect(btn, "clicked", G_CALLBACK(notebook_msgwin_tab_close_clicked_cb), window);

    /* line, doc, doc_filename, markup */
	store = gtk_list_store_new(4, G_TYPE_INT, G_TYPE_POINTER, G_TYPE_STRING, G_TYPE_STRING);

	gtk_tree_view_set_model(GTK_TREE_VIEW(treeview), GTK_TREE_MODEL(store));
	g_object_unref(store);

	renderer = gtk_cell_renderer_text_new();
	column = gtk_tree_view_column_new_with_attributes(NULL, renderer,
		"markup", STORE_MSG_MARKUP_INDEX, NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(treeview), column);

	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(treeview), FALSE);

	ui_widget_modify_font_from_string(treeview, interface_prefs.msgwin_font);

	/* use button-release-event so the selection has changed
	 * (connect_after button-press-event doesn't work) */
	g_signal_connect(treeview, "button-release-event",
					G_CALLBACK(on_msgwin_button_press_event), GINT_TO_POINTER(MSG_MESSAGE));
	/* for double-clicking only, after the first release */
	g_signal_connect(treeview, "button-press-event",
					G_CALLBACK(on_msgwin_button_press_event), GINT_TO_POINTER(MSG_MESSAGE));
	g_signal_connect(treeview, "key-press-event",
		G_CALLBACK(on_msgwin_key_press_event), GINT_TO_POINTER(MSG_MESSAGE));

	/* selection handling */
	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
	gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
	/*g_signal_connect(selection, "changed",G_CALLBACK(on_msg_tree_selection_changed), NULL);*/

	gtk_container_add(GTK_CONTAINER(window), treeview);

   	gtk_widget_show_all(hbox);
	gtk_notebook_append_page(GTK_NOTEBOOK(msgwindow.msg_notebook), window, hbox);

	/* goto last page */
	gtk_notebook_set_current_page(GTK_NOTEBOOK(msgwindow.msg_notebook), - 1);
	
	/* do not open too many (XXX: add a setting for that) */
	if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(msgwindow.msg_notebook)) > 50) {
		gtk_notebook_remove_page(GTK_NOTEBOOK(msgwindow.msg_notebook), 0); 
	}

	g_object_set_data_full(G_OBJECT(treeview), MESSAGES_DIR_KEY, g_strdup(messages_dir), g_free);
    
    /* remember global values */
	g_free(msgwindow.messages_dir);
	msgwindow.messages_dir = g_strdup(messages_dir);
	msgwindow.tree_msg = treeview;
	msgwindow.store_msg = store;
}

void msgwin_init(void)
{
	msgwindow.notebook = ui_lookup_widget(main_widgets.window, "notebook_info");
	msgwindow.tree_status = ui_lookup_widget(main_widgets.window, "treeview3");
	msgwindow.msg_notebook = ui_lookup_widget(main_widgets.window, "notebook_msg");
	msgwindow.tree_compiler = ui_lookup_widget(main_widgets.window, "treeview5");
	msgwindow.scribble = ui_lookup_widget(main_widgets.window, "textview_scribble");
	msgwindow.messages_dir = NULL;

	prepare_status_tree_view();
	prepare_compiler_tree_view();

	/* add the default message treeview */
	msgwin_add_page(_("Messages"), NULL);

	msgwindow.popup_status_menu = create_message_popup_menu(MSG_STATUS);
	msgwindow.popup_msg_menu = create_message_popup_menu(MSG_MESSAGE);
	msgwindow.popup_compiler_menu = create_message_popup_menu(MSG_COMPILER);

	ui_widget_modify_font_from_string(msgwindow.scribble, interface_prefs.msgwin_font);
	g_signal_connect(msgwindow.scribble, "populate-popup", G_CALLBACK(on_scribble_populate), NULL);
}


void msgwin_finalize(void)
{
	g_free(msgwindow.messages_dir);
}


static gboolean on_msgwin_key_press_event(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
	gboolean enter_or_return = ui_is_keyval_enter_or_return(event->keyval);

	if (enter_or_return || event->keyval == GDK_space)
	{
		switch (GPOINTER_TO_INT(data))
		{
			case MSG_COMPILER:
			{	/* key press in the compiler treeview */
				msgwin_goto_compiler_file_line(widget, enter_or_return);
				break;
			}
			case MSG_MESSAGE:
			{	/* key press in the message treeview (results of 'Find usage') */
				msgwin_goto_messages_file_line(widget, enter_or_return);
				break;
			}
		}
	}
	return FALSE;
}


/* does some preparing things to the status message list widget */
static void prepare_status_tree_view(void)
{
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;

	msgwindow.store_status = gtk_list_store_new(1, G_TYPE_STRING);
	gtk_tree_view_set_model(GTK_TREE_VIEW(msgwindow.tree_status), GTK_TREE_MODEL(msgwindow.store_status));
	g_object_unref(msgwindow.store_status);

	renderer = gtk_cell_renderer_text_new();
	column = gtk_tree_view_column_new_with_attributes(_("Status messages"), renderer, "text", 0, NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(msgwindow.tree_status), column);

	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(msgwindow.tree_status), FALSE);

	ui_widget_modify_font_from_string(msgwindow.tree_status, interface_prefs.msgwin_font);

	g_signal_connect(msgwindow.tree_status, "button-press-event",
				G_CALLBACK(on_msgwin_button_press_event), GINT_TO_POINTER(MSG_STATUS));
}

/* does some preparing things to the compiler list widget */
static void prepare_compiler_tree_view(void)
{
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;
	GtkTreeSelection *selection;

	msgwindow.store_compiler = gtk_list_store_new(2, GDK_TYPE_COLOR, G_TYPE_STRING);
	gtk_tree_view_set_model(GTK_TREE_VIEW(msgwindow.tree_compiler), GTK_TREE_MODEL(msgwindow.store_compiler));
	g_object_unref(msgwindow.store_compiler);

	renderer = gtk_cell_renderer_text_new();
	column = gtk_tree_view_column_new_with_attributes(NULL, renderer, "foreground-gdk", 0, "text", 1, NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(msgwindow.tree_compiler), column);

	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(msgwindow.tree_compiler), FALSE);

	ui_widget_modify_font_from_string(msgwindow.tree_compiler, interface_prefs.msgwin_font);

	/* use button-release-event so the selection has changed
	 * (connect_after button-press-event doesn't work) */
	g_signal_connect(msgwindow.tree_compiler, "button-release-event",
					G_CALLBACK(on_msgwin_button_press_event), GINT_TO_POINTER(MSG_COMPILER));
	/* for double-clicking only, after the first release */
	g_signal_connect(msgwindow.tree_compiler, "button-press-event",
					G_CALLBACK(on_msgwin_button_press_event), GINT_TO_POINTER(MSG_COMPILER));
	g_signal_connect(msgwindow.tree_compiler, "key-press-event",
		G_CALLBACK(on_msgwin_key_press_event), GINT_TO_POINTER(MSG_COMPILER));

	/* selection handling */
	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(msgwindow.tree_compiler));
	gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
	/*g_signal_connect(selection, "changed", G_CALLBACK(on_msg_tree_selection_changed), NULL);*/
}


static const GdkColor color_error = {0, 65535, 0, 0};

static const GdkColor *get_color(gint msg_color)
{
	static const GdkColor dark_red = {0, 65535 / 2, 0, 0};
	static const GdkColor blue = {0, 0, 0, 0xD000};	/* not too bright ;-) */
	static const GdkColor black = {0, 0, 0, 0};

	switch (msg_color)
	{
		case COLOR_RED: return &color_error;
		case COLOR_DARK_RED: return &dark_red;
		case COLOR_BLUE: return &blue;
		case COLOR_BLACK:
		default:
			return &black;
	}
}


/**
 *  Adds a new message in the compiler tab treeview in the messages window.
 *
 *  @param msg_color A color to be used for the text. It must be an element of #MsgColors.
 *  @param format @c printf()-style format string.
 *  @param ... Arguments for the @c format string.
 **/
void msgwin_compiler_add(gint msg_color, const gchar *format, ...)
{
	gchar *string;
	va_list args;

	va_start(args, format);
	string = g_strdup_vprintf(format, args);
	va_end(args);
	msgwin_compiler_add_string(msg_color, string);
	g_free(string);
}


void msgwin_compiler_add_string(gint msg_color, const gchar *msg)
{
	GtkTreeIter iter;
	GtkTreePath *path;
	const GdkColor *color = get_color(msg_color);
	gchar *utf8_msg;

	if (! g_utf8_validate(msg, -1, NULL))
		utf8_msg = utils_get_utf8_from_locale(msg);
	else
		utf8_msg = (gchar *) msg;

	gtk_list_store_append(msgwindow.store_compiler, &iter);
	gtk_list_store_set(msgwindow.store_compiler, &iter, 0, color, 1, utf8_msg, -1);

	if (ui_prefs.msgwindow_visible && interface_prefs.compiler_tab_autoscroll)
	{
		path = gtk_tree_model_get_path(
			gtk_tree_view_get_model(GTK_TREE_VIEW(msgwindow.tree_compiler)), &iter);
		gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(msgwindow.tree_compiler), path, NULL, TRUE, 0.5, 0.5);
		gtk_tree_path_free(path);
	}

	/* calling build_menu_update for every build message would be overkill, TODO really should call it once when all done */
	gtk_widget_set_sensitive(build_get_menu_items(-1)->menu_item[GBG_FIXED][GBF_NEXT_ERROR], TRUE);
	gtk_widget_set_sensitive(build_get_menu_items(-1)->menu_item[GBG_FIXED][GBF_PREV_ERROR], TRUE);

	if (utf8_msg != msg)
		g_free(utf8_msg);
}


void msgwin_show_hide(gboolean show)
{
	ui_prefs.msgwindow_visible = show;
	ignore_callback = TRUE;
	gtk_check_menu_item_set_active(
		GTK_CHECK_MENU_ITEM(ui_lookup_widget(main_widgets.window, "menu_show_messages_window1")),
		show);
	ignore_callback = FALSE;
	ui_widget_show_hide(ui_lookup_widget(main_widgets.window, "scrolledwindow1"), show);
	/* set the input focus back to the editor */
	keybindings_send_command(GEANY_KEY_GROUP_FOCUS, GEANY_KEYS_FOCUS_EDITOR);
}


/**
 *  Adds a new message in the messages tab treeview in the messages window.
 *  If @a line and @a doc are set, clicking on this line jumps into the file which is specified
 *  by @a doc into the line specified with @a line.
 *
 *  @param msg_color A color to be used for the text. It must be an element of #MsgColors.
 *  @param line The document's line where the message belongs to. Set to @c -1 to ignore.
 *  @param doc The document. Set to @c NULL to ignore.
 *  @param format @c printf()-style format string.
 *  @param ... Arguments for the @c format string.
 *
 * @since 0.15
 **/
void msgwin_msg_add(gint msg_color, gint line, GeanyDocument *doc, const gchar *format, ...)
{
	gchar *string;
	va_list args;

	va_start(args, format);
	string = g_strdup_vprintf(format, args);
	va_end(args);

	msgwin_msg_add_string(msg_color, line, doc, string);
	g_free(string);
}


/**
 *
 *  @param string the string to display. If it is not utf8, it will be turned into
 * a valid utf8 string at the price of losing some chars
 **/
void msgwin_msg_add_string(gint msg_color, gint line, GeanyDocument *doc, const gchar *string)
{
	const GdkColor *color = get_color(msg_color);
	GString *gs;
	gchar *escaped;
	const gchar *utf8;

	if (! g_utf8_validate(string, -1, NULL))
		utf8 = utils_utf8_make_valid(string, NULL);
	else
		utf8 = string;

	escaped = g_markup_escape_text(utf8, -1);
	gs = g_string_new("");
	g_string_printf(gs, "<span foreground=\"%s\">%s</span>", gdk_color_to_string(color), escaped);

	msgwin_msg_add_markup_no_validate(line, doc, NULL, gs->str);

	if (utf8 != string)
	{
		g_free((gchar*)utf8);
	}
	
	g_string_free(gs, TRUE);
	g_free(escaped);
}

typedef struct {
	 GQueue *element_queue;
	 GString *result_string;
	 GString *text;
	 gboolean done;
	 gsize max_len;
} ParserData;

/*
 * This is called on element boundaries so that we put back escape text
 */
static void append_text(ParserData *data)
{
	gchar *tmp;
	const gchar *end;

	if (data->text == NULL)
		return;

	/* in case we cut in the middle of a UTF8 char */
	g_utf8_validate(data->text->str, data->text->len, &end);

	tmp = g_markup_escape_text(data->text->str, (end - data->text->str));
	g_string_append(data->result_string, tmp);

	g_string_free(data->text, TRUE);
	data->text = NULL;
}

static void parser_start_element (GMarkupParseContext *context,
									const gchar         *element_name,
									const gchar        **attribute_names,
									const gchar        **attribute_values,
									gpointer             user_data,
									GError             **error)
{
	ParserData *data = (ParserData*)user_data;
	gchar *element;

	if (data->done)
	{
		return;
	}

	append_text(data);

	g_string_append_printf(data->result_string, "<%s", element_name);
	while (*attribute_names && *attribute_values)
	{
		g_string_append_printf(data->result_string, " %s=\"%s\"", *attribute_names, *attribute_values);

		attribute_names++;
		attribute_values++;
	}
	g_string_append(data->result_string, ">");
	
	element = g_strdup(element_name);
	g_queue_push_tail(data->element_queue, element);
}

static void parser_end_element (GMarkupParseContext *context,
								const gchar         *element_name,
								gpointer             user_data,
								GError             **error)
{
	ParserData *data = (ParserData*)user_data;
	gchar *element;

	if (data->done)
		return;

	append_text(data);
	g_string_append_printf(data->result_string, "</%s>", element_name);

	element = g_queue_pop_tail(data->element_queue);

	g_assert(!strcmp(element, element_name));
	g_free(element);
}


static void parser_text (GMarkupParseContext *context,
						const gchar         *text,
						gsize                text_len,
						gpointer             user_data,
						GError             **error)
{
	ParserData *data = (ParserData*)user_data;
	gsize sofar;

	if (data->done)
		return;

	if (data->text == NULL)
		data->text = g_string_new("");
	
	sofar = data->text->len + data->result_string->len;
	
	if (text_len > data->max_len - sofar)
	{
		data->done = TRUE;
		g_string_append_len(data->text, text, data->max_len - sofar);
		append_text(data);
	}
	else
	{
		g_string_append_len(data->text, text, text_len);
	}
}

static void parser_error (GMarkupParseContext *context,
							GError              *error,
							gpointer             user_data)
{
	ParserData *data = (ParserData*)user_data;
	if (data->done)
		return;

	data->done = TRUE;
}

/*
 *  Shorten a markup string. It will ensure that open tags are closed.
 * It will also strip comments
 *
 *  @param markup the markup to shorten
 *  @param len the maximum length of text of the resulting markup
 * this does not take into account the tags themselves
 */
static gchar *shorten_markup(const gchar *markup, gsize len)
{
	GMarkupParser parser;
	ParserData data;
	GMarkupParseContext *context;
	gchar *element;

	parser.start_element = parser_start_element;
	parser.end_element = parser_end_element;
	parser.text = parser_text;
	parser.error = parser_error;

	data.element_queue = g_queue_new();
	data.result_string = g_string_new("");
	data.text = NULL;
	data.done = FALSE;
	data.max_len = len;

	context = g_markup_parse_context_new(&parser, G_MARKUP_TREAT_CDATA_AS_TEXT, &data, NULL);
	/* we need to enclose everything in a root element for the parser to parse it correctly*/
	g_markup_parse_context_parse(context, "<span>", -1, NULL);
	g_markup_parse_context_parse(context, markup, -1, NULL);
	g_markup_parse_context_parse(context, "</span>", -1, NULL);
	
	/* now close the open tags */
	while ((element = g_queue_pop_tail(data.element_queue)))
	{
		g_string_append_printf(data.result_string, "</%s>", element);
		g_free(element);
	}
	
	g_queue_free(data.element_queue);
	g_markup_parse_context_free(context);

	return g_string_free(data.result_string, FALSE);
}

/*
 * XXX: having both doc *and* doc_filename seems a bit redundant to me.
 */
void msgwin_msg_add_markup_no_validate(gint line, GeanyDocument *doc, const gchar *doc_filename,
												const gchar *utf8_msg)
{
	GtkTreeIter iter;
	const gchar *tmp;
	gsize len;

	if (! ui_prefs.msgwindow_visible)
		msgwin_show_hide(TRUE);

	/* work around a strange problem when adding very long lines(greater than 4000 bytes)
	 * cut the string to a maximum of 1024 bytes and discard the rest.
	 * the test for 'len > 1024' is a bit too permissive as it takes potential tags into account */
	/* TODO: find the real cause for the display problem / if it is GtkTreeView file a bug report */
	len = strlen(utf8_msg);
	if (len > 1024)
		tmp = shorten_markup(utf8_msg, 1024);
	else
		tmp = utf8_msg;
	
	gtk_list_store_append(msgwindow.store_msg, &iter);
	gtk_list_store_set(msgwindow.store_msg, &iter, STORE_MSG_LINE_INDEX, line,
						STORE_MSG_DOC_INDEX, doc, STORE_MSG_DOC_FILENAME_INDEX, doc_filename,
						STORE_MSG_MARKUP_INDEX, tmp, -1);

	if (tmp != utf8_msg)
		g_free((gchar*)tmp);
}


/**
 *  Logs a status message *without* setting the status bar.
 *  (Use ui_set_statusbar() to display text on the statusbar)
 *
 *  @param format @c printf()-style format string.
 *  @param ... Arguments for the @c format string.
 **/
void msgwin_status_add(const gchar *format, ...)
{
	GtkTreeIter iter;
	gchar *string;
	gchar *statusmsg, *time_str;
	va_list args;

	va_start(args, format);
	string = g_strdup_vprintf(format, args);
	va_end(args);

	/* add a timestamp to status messages */
	time_str = utils_get_current_time_string();
	statusmsg = g_strconcat(time_str, ": ", string, NULL);
	g_free(time_str);
	g_free(string);

	/* add message to Status window */
	gtk_list_store_append(msgwindow.store_status, &iter);
	gtk_list_store_set(msgwindow.store_status, &iter, 0, statusmsg, -1);
	g_free(statusmsg);

	if (G_LIKELY(main_status.main_window_realized))
	{
		GtkTreePath *path = gtk_tree_model_get_path(gtk_tree_view_get_model(GTK_TREE_VIEW(msgwindow.tree_status)), &iter);

		gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(msgwindow.tree_status), path, NULL, FALSE, 0.0, 0.0);
		if (prefs.switch_to_status)
			gtk_notebook_set_current_page(GTK_NOTEBOOK(msgwindow.notebook), MSG_STATUS);
		gtk_tree_path_free(path);
	}
}


static void
on_message_treeview_clear_activate(GtkMenuItem *menuitem, gpointer user_data)
{
	gint tabnum = GPOINTER_TO_INT(user_data);

	msgwin_clear_tab(tabnum);
}


static void
on_compiler_treeview_copy_activate(GtkMenuItem *menuitem, gpointer user_data)
{
	GtkWidget *tv = NULL;
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GtkTreeIter iter;
	gint str_idx = 1;
	gboolean needs_parse = FALSE;

	switch (GPOINTER_TO_INT(user_data))
	{
		case MSG_STATUS:
		tv = msgwindow.tree_status;
		str_idx = 0;
		break;

		case MSG_COMPILER:
		tv = msgwindow.tree_compiler;
		break;

		case MSG_MESSAGE:
		tv = msgwindow.tree_msg;
		str_idx = STORE_MSG_MARKUP_INDEX;
		needs_parse = TRUE;
		break;
	}
	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tv));

	if (gtk_tree_selection_get_selected(selection, &model, &iter))
	{
		gchar *string;

		gtk_tree_model_get(model, &iter, str_idx, &string, -1);
		if (NZV(string))
		{
			if (needs_parse)
			{
				gchar *tmp;
				if (pango_parse_markup(string, -1, 0, NULL, &tmp, NULL, NULL) == TRUE && tmp)
				{
					g_free(string);
					string = tmp;
				}
			}
			
			gtk_clipboard_set_text(gtk_clipboard_get(gdk_atom_intern("CLIPBOARD", FALSE)),
				string, -1);
		}
		g_free(string);
	}
}


static void on_compiler_treeview_copy_all_activate(GtkMenuItem *menuitem, gpointer user_data)
{
	GtkListStore *store = msgwindow.store_compiler;
	GtkTreeIter iter;
	GString *str = g_string_new("");
	gint str_idx = 1;
	gboolean valid;
	gboolean needs_parse = FALSE;

	switch (GPOINTER_TO_INT(user_data))
	{
		case MSG_STATUS:
		store = msgwindow.store_status;
		str_idx = 0;
		break;

		case MSG_COMPILER:
		/* default values */
		break;

		case MSG_MESSAGE:
		store = msgwindow.store_msg;
		str_idx = STORE_MSG_MARKUP_INDEX;
		needs_parse = TRUE;
		break;
	}

	/* walk through the list and copy every line into a string */
	valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter);
	while (valid)
	{
		gchar *line;

		gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, str_idx, &line, -1);
		if (NZV(line))
		{
			if (needs_parse)
			{
				gchar *tmp;
				if (pango_parse_markup(line, -1, 0, NULL, &tmp, NULL, NULL) == TRUE && tmp)
				{
					g_free(line);
					line = tmp;
				}
			}
			g_string_append(str, line);
			g_string_append_c(str, '\n');
		}
		g_free(line);

		valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter);
	}

	/* copy the string into the clipboard */
	if (str->len > 0)
	{
		gtk_clipboard_set_text(
			gtk_clipboard_get(gdk_atom_intern("CLIPBOARD", FALSE)),
			str->str,
			str->len);
	}
	g_string_free(str, TRUE);
}


static void
on_hide_message_window(GtkMenuItem *menuitem, gpointer user_data)
{
	msgwin_show_hide(FALSE);
}


static GtkWidget *create_message_popup_menu(gint type)
{
	GtkWidget *message_popup_menu, *clear, *copy, *copy_all, *image;

	message_popup_menu = gtk_menu_new();

	clear = gtk_image_menu_item_new_from_stock(GTK_STOCK_CLEAR, NULL);
	gtk_widget_show(clear);
	gtk_container_add(GTK_CONTAINER(message_popup_menu), clear);
	g_signal_connect(clear, "activate",
		G_CALLBACK(on_message_treeview_clear_activate), GINT_TO_POINTER(type));

	copy = gtk_image_menu_item_new_with_mnemonic(_("C_opy"));
	gtk_widget_show(copy);
	gtk_container_add(GTK_CONTAINER(message_popup_menu), copy);
	image = gtk_image_new_from_stock(GTK_STOCK_COPY, GTK_ICON_SIZE_MENU);
	gtk_widget_show(image);
	gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(copy), image);
	g_signal_connect(copy, "activate",
		G_CALLBACK(on_compiler_treeview_copy_activate), GINT_TO_POINTER(type));

	copy_all = gtk_image_menu_item_new_with_mnemonic(_("Copy _All"));
	gtk_widget_show(copy_all);
	gtk_container_add(GTK_CONTAINER(message_popup_menu), copy_all);
	image = gtk_image_new_from_stock(GTK_STOCK_COPY, GTK_ICON_SIZE_MENU);
	gtk_widget_show(image);
	gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(copy_all), image);
	g_signal_connect(copy_all, "activate",
		G_CALLBACK(on_compiler_treeview_copy_all_activate), GINT_TO_POINTER(type));

	msgwin_menu_add_common_items(GTK_MENU(message_popup_menu));

	return message_popup_menu;
}


static void on_scribble_populate(GtkTextView *textview, GtkMenu *arg1, gpointer user_data)
{
	msgwin_menu_add_common_items(arg1);
}


/* Menu items that should be on all message window popup menus */
void msgwin_menu_add_common_items(GtkMenu *menu)
{
	GtkWidget *item;

	item = gtk_separator_menu_item_new();
	gtk_widget_show(item);
	gtk_container_add(GTK_CONTAINER(menu), item);

	item = gtk_menu_item_new_with_mnemonic(_("_Hide Message Window"));
	gtk_widget_show(item);
	gtk_container_add(GTK_CONTAINER(menu), item);
	g_signal_connect(item, "activate", G_CALLBACK(on_hide_message_window), NULL);
}


/* look back up from the current path and find the directory we came from */
static gboolean
find_prev_build_dir(GtkTreePath *cur, GtkTreeModel *model, gchar **prefix)
{
	GtkTreeIter iter;
	*prefix = NULL;

	while (gtk_tree_path_prev(cur))
	{
		if (gtk_tree_model_get_iter(model, &iter, cur))
		{
			gchar *string;
			gtk_tree_model_get(model, &iter, 1, &string, -1);
			if (string != NULL && build_parse_make_dir(string, prefix))
			{
				g_free(string);
				return TRUE;
			}
			g_free(string);
		}
	}

	return FALSE;
}


static gboolean goto_compiler_file_line(const gchar *filename, gint line, gboolean focus_editor)
{
	if (!filename || line <= -1)
		return FALSE;

	/* If the path doesn't exist, try the current document.
	 * This happens when we receive build messages in the wrong order - after the
	 * 'Leaving directory' messages */
	if (!g_file_test(filename, G_FILE_TEST_EXISTS))
	{
		gchar *cur_dir = utils_get_current_file_dir_utf8();
		gchar *name;

		if (cur_dir)
		{
			/* we let the user know we couldn't find the parsed filename from the message window */
			SETPTR(cur_dir, utils_get_locale_from_utf8(cur_dir));
			name = g_path_get_basename(filename);
			SETPTR(name, g_build_path(G_DIR_SEPARATOR_S, cur_dir, name, NULL));
			g_free(cur_dir);

			if (g_file_test(name, G_FILE_TEST_EXISTS))
			{
				ui_set_statusbar(FALSE, _("Could not find file '%s' - trying the current document path."),
					filename);
				filename = name;
			}
			else
				g_free(name);
		}
	}

	{
		gchar *utf8_filename = utils_get_utf8_from_locale(filename);
		GeanyDocument *doc = document_find_by_filename(utf8_filename);
		GeanyDocument *old_doc = document_get_current();

		g_free(utf8_filename);

		if (doc == NULL)	/* file not already open */
			doc = document_open_file(filename, FALSE, NULL, NULL);

		if (doc != NULL)
		{
			gboolean ret;

			if (! doc->changed && editor_prefs.use_indicators)	/* if modified, line may be wrong */
				editor_indicator_set_on_line(doc->editor, GEANY_INDICATOR_ERROR, line - 1);

			ret = navqueue_goto_line(old_doc, doc, line);
			if (ret && focus_editor)
				gtk_widget_grab_focus(GTK_WIDGET(doc->editor->sci));

			return ret;
		}
	}
	return FALSE;
}


gboolean msgwin_goto_compiler_file_line(GtkWidget *widget, gboolean focus_editor)
{
	GtkTreeIter iter;
	GtkTreeModel *model;
	GtkTreeSelection *selection;
	gchar *string;
	GdkColor *color;

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(msgwindow.tree_compiler));
	if (gtk_tree_selection_get_selected(selection, &model, &iter))
	{
		/* if the item is not coloured red, it's not an error line */
		gtk_tree_model_get(model, &iter, 0, &color, -1);
		if (color == NULL || ! gdk_color_equal(color, &color_error))
		{
			if (color != NULL)
				gdk_color_free(color);
			return FALSE;
		}
		gdk_color_free(color);

		gtk_tree_model_get(model, &iter, 1, &string, -1);
		if (string != NULL)
		{
			gint line;
			gchar *filename, *dir;
			GtkTreePath *path;
			gboolean ret;

			path = gtk_tree_model_get_path(model, &iter);
			find_prev_build_dir(path, model, &dir);
			gtk_tree_path_free(path);
			msgwin_parse_compiler_error_line(string, dir, &filename, &line);
			g_free(string);
			g_free(dir);

			ret = goto_compiler_file_line(filename, line, focus_editor);
			g_free(filename);
			return ret;
		}
	}
	return FALSE;
}


static void make_absolute(gchar **filename, const gchar *dir)
{
	guint skip_dot_slash = 0;	/* number of characters to skip at the beginning of the filename */

	if (*filename == NULL)
		return;

	/* skip some characters at the beginning of the filename, at the moment only "./"
	 * can be extended if other "trash" is known */
	if (strncmp(*filename, "./", 2) == 0)
		skip_dot_slash = 2;

	/* add directory */
	if (! utils_is_absolute_path(*filename))
		SETPTR(*filename, g_build_filename(dir, *filename + skip_dot_slash, NULL));
}


/* try to parse the file and line number where the error occured described in line
 * and when something useful is found, it stores the line number in *line and the
 * relevant file with the error in *filename.
 * *line will be -1 if no error was found in string.
 * *filename must be freed unless it is NULL. */
static void parse_file_line(ParseData *data, gchar **filename, gint *line)
{
	gchar *end = NULL;
	gchar **fields;

	*filename = NULL;
	*line = -1;

	g_return_if_fail(data->string != NULL);

	fields = g_strsplit_set(data->string, data->pattern, data->min_fields);

	/* parse the line */
	if (g_strv_length(fields) < data->min_fields)
	{
		g_strfreev(fields);
		return;
	}

	*line = strtol(fields[data->line_idx], &end, 10);

	/* if the line could not be read, line is 0 and an error occurred, so we leave */
	if (fields[data->line_idx] == end)
	{
		g_strfreev(fields);
		return;
	}

	/* let's stop here if there is no filename in the error message */
	if (data->file_idx == -1)
	{
		/* we have no filename in the error message, so take the current one and hope it's correct */
		GeanyDocument *doc = document_get_current();
		if (doc != NULL)
			*filename = g_strdup(doc->file_name);
		g_strfreev(fields);
		return;
	}

	*filename = g_strdup(fields[data->file_idx]);
	g_strfreev(fields);
}


static void parse_compiler_error_line(const gchar *string,
		gchar **filename, gint *line)
{
	ParseData data = {NULL, NULL, 0, 0, 0};

	data.string = string;

	switch (build_info.file_type_id)
	{
		case GEANY_FILETYPES_PHP:
		{
			/* Parse error: parse error, unexpected T_CASE in brace_bug.php on line 3
			 * Parse error: syntax error, unexpected T_LNUMBER, expecting T_FUNCTION in bob.php on line 16 */
			gchar *tmp = strstr(string, " in ");

			if (tmp != NULL)
			{
				data.string = tmp;
				data.pattern = " ";
				data.min_fields = 6;
				data.line_idx = 5;
				data.file_idx = 2;
			}
			else
			{
				data.pattern = " ";
				data.min_fields = 11;
				data.line_idx = 10;
				data.file_idx = 7;
			}
			break;
		}
		case GEANY_FILETYPES_PERL:
		{
			/* syntax error at test.pl line 7, near "{ */
			data.pattern = " ";
			data.min_fields = 6;
			data.line_idx = 5;
			data.file_idx = 3;
			break;
		}
		/* the error output of python and tcl equals */
		case GEANY_FILETYPES_TCL:
		case GEANY_FILETYPES_PYTHON:
		{
			/* File "HyperArch.py", line 37, in ?
			 * (file "clrdial.tcl" line 12)
			 * */
			if (strstr(string, " line ") != NULL)
			{
				/* Tcl and old Python format (<= Python 2.5) */
				data.pattern = " \"";
				data.min_fields = 6;
				data.line_idx = 5;
				data.file_idx = 2;
			}
			else
			{
				/* SyntaxError: ('invalid syntax', ('sender.py', 149, 20, ' ...'))
				 * (used since Python 2.6) */
				data.pattern = ",'";
				data.min_fields = 8;
				data.line_idx = 6;
				data.file_idx = 4;
			}
			break;
		}
		case GEANY_FILETYPES_BASIC:
		case GEANY_FILETYPES_PASCAL:
		case GEANY_FILETYPES_CS:
		{
			/* getdrive.bas(52) error 18: Syntax error in '? GetAllDrives'
			 * bandit.pas(149,3) Fatal: Syntax error, ";" expected but "ELSE" found */
			data.pattern = "(";
			data.min_fields = 2;
			data.line_idx = 1;
			data.file_idx = 0;
			break;
		}
		case GEANY_FILETYPES_D:
		{
			/* GNU D compiler front-end, gdc
			 * gantry.d:18: variable gantry.main.c reference to auto class must be auto
			 * warning - gantry.d:20: statement is not reachable
			 * Digital Mars dmd compiler
			 * warning - pi.d(118): implicit conversion of expression (digit) of type int ...
			 * gantry.d(18): variable gantry.main.c reference to auto class must be auto */
			if (strncmp(string, "warning - ", 10) == 0)
			{
				data.pattern = " (:";
				data.min_fields = 4;
				data.line_idx = 3;
				data.file_idx = 2;
			}
			else
			{
				data.pattern = "(:";
				data.min_fields = 2;
				data.line_idx = 1;
				data.file_idx = 0;
			}
			break;
		}
		case GEANY_FILETYPES_FERITE:
		{
			/* Error: Parse Error: on line 5 in "/tmp/hello.fe"
			 * Error: Compile Error: on line 24, in /test/class.fe */
			if (strncmp(string, "Error: Compile Error", 20) == 0)
			{
				data.pattern = " ";
				data.min_fields = 8;
				data.line_idx = 5;
				data.file_idx = 7;
			}
			else
			{
				data.pattern = " \"";
				data.min_fields = 10;
				data.line_idx = 5;
				data.file_idx = 8;
			}
			break;
		}
		case GEANY_FILETYPES_HTML:
		{
			/* line 78 column 7 - Warning: <table> missing '>' for end of tag */
			data.pattern = " ";
			data.min_fields = 4;
			data.line_idx = 1;
			data.file_idx = -1;
			break;
		}
		/* All GNU gcc-like error messages */
		case GEANY_FILETYPES_C:
		case GEANY_FILETYPES_CPP:
		case GEANY_FILETYPES_RUBY:
		case GEANY_FILETYPES_JAVA:
			/* only gcc is supported, I don't know any other C(++) compilers and their error messages
			 * empty.h:4: Warnung: type defaults to `int' in declaration of `foo'
			 * empty.c:21: error: conflicting types for `foo'
			 * Only parse file and line, so that linker errors will also work (with -g) */
		case GEANY_FILETYPES_F77:
		case GEANY_FILETYPES_FORTRAN:
		case GEANY_FILETYPES_LATEX:
			/* ./kommtechnik_2b.tex:18: Emergency stop. */
		case GEANY_FILETYPES_MAKE:	/* Assume makefile is building with gcc */
		case GEANY_FILETYPES_NONE:
		default:	/* The default is a GNU gcc type error */
		{
			if (build_info.file_type_id == GEANY_FILETYPES_JAVA &&
				strncmp(string, "[javac]", 7) == 0)
			{
				/* Java Apache Ant.
				 * [javac] <Full Path to File + extension>:<line n°>: <error> */
				data.pattern = " :";
				data.min_fields = 4;
				data.line_idx = 2;
				data.file_idx = 1;
				break;
			}
			/* don't accidently find libtool versions x:y:x and think it is a file name */
			if (strstr(string, "libtool --mode=link") == NULL)
			{
				data.pattern = ":";
				data.min_fields = 3;
				data.line_idx = 1;
				data.file_idx = 0;
				break;
			}
		}
	}

	if (data.pattern != NULL)
		parse_file_line(&data, filename, line);
}


/* try to parse the file and line number where the error occured described in string
 * and when something useful is found, it stores the line number in *line and the
 * relevant file with the error in *filename.
 * *line will be -1 if no error was found in string.
 * *filename must be freed unless it is NULL. */
void msgwin_parse_compiler_error_line(const gchar *string, const gchar *dir,
		gchar **filename, gint *line)
{
	GeanyFiletype *ft;
	gchar *trimmed_string;

	*filename = NULL;
	*line = -1;

	if (G_UNLIKELY(string == NULL))
		return;

	if (dir == NULL)
		dir = build_info.dir;
	g_return_if_fail(dir != NULL);

	trimmed_string = g_strdup(string);
	g_strchug(trimmed_string); /* remove possible leading whitespace */

	ft = filetypes[build_info.file_type_id];

	/* try parsing with a custom regex */
	if (!filetypes_parse_error_message(ft, trimmed_string, filename, line))
	{
		/* fallback to default old-style parsing */
		parse_compiler_error_line(trimmed_string, filename, line);
	}
	make_absolute(filename, dir);
	g_free(trimmed_string);
}


/* Tries to parse strings of the file:line style, allowing line field to be missing
 * * filename is filled with the filename, should be freed
 * * line is filled with the line number or -1 */
static void msgwin_parse_generic_line(const gchar *string, gchar **filename, gint *line)
{
	gchar **fields;
	gboolean incertain = TRUE; /* whether we're reasonably certain of the result */

	*filename = NULL;
	*line = -1;

	fields = g_strsplit(string, ":", 2);
	/* extract the filename */
	if (fields[0] != NULL)
	{
		*filename = g_strdup(fields[0]);

		/* now the line */
		if (fields[1] != NULL)
		{
			gchar *end;

			*line = strtol(fields[1], &end, 10);
			if (end == fields[1])
				*line = -1;
			else if (*end == ':' || g_ascii_isspace(*end))
			{	/* if we have a blank or a separator right after the number, assume we really got a
				 * filename (it's a grep-like syntax) */
				incertain = FALSE;
			}
		}

		/* if we aren't sure we got a supposedly correct filename, check it */
		if (incertain && ! g_file_test(*filename, G_FILE_TEST_EXISTS))
		{
			SETPTR(*filename, NULL);
			*line = -1;
		}
	}
	g_strfreev(fields);
}


/* return TRUE if a message was found */
gboolean msgwin_goto_messages_file_line(GtkWidget *widget, gboolean focus_editor)
{
	GtkTreeIter iter;
	GtkTreeModel *model;
	GtkTreeSelection *selection;
	gboolean ret = FALSE;
    gchar *messages_dir = g_object_get_data(G_OBJECT(widget), MESSAGES_DIR_KEY);
	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
	if (gtk_tree_selection_get_selected(selection, &model, &iter))
	{
		gint line;
		gchar *markup;
		GeanyDocument *doc;
		gchar *filename;
		GeanyDocument *old_doc = document_get_current();
		gint line2 = -1;

		gtk_tree_model_get(model, &iter, STORE_MSG_LINE_INDEX, &line,
							STORE_MSG_DOC_INDEX, &doc,
							STORE_MSG_DOC_FILENAME_INDEX, &filename,
							STORE_MSG_MARKUP_INDEX, &markup,
							-1);
		
		if (doc != NULL && !doc->is_valid)
		{
			/* doc has been closed, do not use it */
			doc = NULL;
		}

		if (doc == NULL)
		{
			if (filename == NULL)
			{
				/* the filename was not provided try with a file:line parsing on the markup */
				gchar *string;
			
				if (pango_parse_markup(markup, -1, 0, NULL, &string, NULL, NULL) == TRUE && string)
				{
					/* try with a file:line parsing */
					msgwin_parse_generic_line(string, &filename, &line2);
                    if (messages_dir != NULL)
                        make_absolute(&filename, messages_dir);
					g_free(string);
				}
			}

			if (filename != NULL)
			{
				if (messages_dir != NULL)
					make_absolute(&filename, messages_dir);
				doc = document_open_file(filename, FALSE, NULL, NULL);
			}
		}
		
		if (doc != NULL)
		{
			/* OK, we found a document */
			if (line < 0 && line2 >= 0)
			{
				/* supplied line is invalid, use the parsed one */
				line = line2;
			}

			ret = (line < 0) ? TRUE : navqueue_goto_line(old_doc, doc, line);
			if (ret && focus_editor)
				gtk_widget_grab_focus(GTK_WIDGET(doc->editor->sci));
		}

		g_free(filename);
		g_free(markup);
	}
	return ret;
}


static gboolean on_msgwin_button_press_event(GtkWidget *widget, GdkEventButton *event,
											 gpointer user_data)
{
	/* user_data might be NULL, GPOINTER_TO_INT returns 0 if called with NULL */
	gboolean double_click = event->type == GDK_2BUTTON_PRESS;

	if (event->button == MSGWIN_BUTTON_LEFT && (event->type == GDK_BUTTON_RELEASE || double_click))
	{
		switch (GPOINTER_TO_INT(user_data))
		{
			case MSG_COMPILER:
			{	/* mouse click in the compiler treeview */
				msgwin_goto_compiler_file_line(widget, double_click);
				break;
			}
			case MSG_MESSAGE:
			{	/* mouse click in the message treeview (results of 'Find usage') */
				msgwin_goto_messages_file_line(widget, double_click);
				break;
			}
		}
		return double_click;	/* TRUE prevents message window re-focusing */
	}

	if (event->button == MSGWIN_BUTTON_RIGHT)
	{	/* popupmenu to hide or clear the active treeview */
		switch (GPOINTER_TO_INT(user_data))
		{
			case MSG_STATUS:
			{
				gtk_menu_popup(GTK_MENU(msgwindow.popup_status_menu), NULL, NULL, NULL, NULL,
																	event->button, event->time);
				break;
			}
			case MSG_MESSAGE:
			{
				gtk_menu_popup(GTK_MENU(msgwindow.popup_msg_menu), NULL, NULL, NULL, NULL,
																	event->button, event->time);
				break;
			}
			case MSG_COMPILER:
			{
				gtk_menu_popup(GTK_MENU(msgwindow.popup_compiler_menu), NULL, NULL, NULL, NULL,
																	event->button, event->time);
				break;
			}
		}
	}
	return FALSE;
}


/**
 *  Switches to the given notebook tab of the messages window and shows the messages window
 *  if it was previously hidden and @a show is set to @c TRUE.
 *
 *  @param tabnum An index of a tab in the messages window. Valid values are all elements of
 *                #MessageWindowTabNum.
 *  @param show Whether to show the messages window at all if it was hidden before.
 *
 * @since 0.15
 **/
void msgwin_switch_tab(gint tabnum, gboolean show)
{
	GtkWidget *widget = NULL;	/* widget to focus */

	switch (tabnum)
	{
		case MSG_SCRATCH: widget = msgwindow.scribble; break;
		case MSG_COMPILER: widget = msgwindow.tree_compiler; break;
		case MSG_STATUS: widget = msgwindow.tree_status; break;
		case MSG_MESSAGE: widget = msgwindow.tree_msg; break;
#ifdef HAVE_VTE
		case MSG_VTE: widget = (vte_info.have_vte) ? vc->vte : NULL; break;
#endif
		default: break;
	}

	/* the msgwin must be visible before we switch to the VTE page so that
	 * the font settings are applied on realization */
	if (show)
		msgwin_show_hide(TRUE);
	gtk_notebook_set_current_page(GTK_NOTEBOOK(msgwindow.notebook), tabnum);
	if (show && widget)
		gtk_widget_grab_focus(widget);
}


/**
 *  Removes all messages from a tab specified by @a tabnum in the messages window.
 *
 *  @param tabnum An index of a tab in the messages window which should be cleared.
 *                Valid values are @c MSG_STATUS, @c MSG_COMPILER and @c MSG_MESSAGE.
 *
 * @since 0.15
 **/
void msgwin_clear_tab(gint tabnum)
{
	GtkListStore *store = NULL;

	switch (tabnum)
	{
		case MSG_MESSAGE:
			store = msgwindow.store_msg;
			break;

		case MSG_COMPILER:
			gtk_list_store_clear(msgwindow.store_compiler);
			build_menu_update(NULL);	/* update next error items */
			return;

		case MSG_STATUS: store = msgwindow.store_status; break;
		default: return;
	}
	if (store == NULL)
		return;
	gtk_list_store_clear(store);
}
