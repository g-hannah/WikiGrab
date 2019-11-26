#include <stdlib.h>
#include <iostream>
#include <list>
#include <map>
#include <gtk/gtk.h>

#define PROG_NAME "WikiGrab"
#define PROG_NAME_DBUS "org.wikigrab"
#define PROG_BUILD "0.0.5"
#define PROG_AUTHORS { "Gary Hannah", (gchar *)NULL }
#define PROG_LICENCE "© Licenced under GNU Library GPLv2"
#define PROG_ICON "./wikigrab_temp_logo.svg"
#define PROG_WEBSITE "https://127.0.0.1/?exists=false%26__not_real__=true"
#define PROG_COMMENTS "Download articles from Wikipedia and\nsave them as text files"

#define WIN_DEFAULT_WIDTH 1000
#define WIN_DEFAULT_HEIGHT 750

static GtkApplication *app;
static GtkWidget *window;
static GtkWidget *grid;

static GtkWidget *toolbar;
static GtkToolItem *tool_item_url;
static GtkWidget *entry_url;
static GtkToolItem *item_button_get;
static GtkWidget *button_get;
static GtkToolItem *item_button_open;
static GtkWidget *button_open;

static GtkWidget *text_area;

#define PROG_ICON_WIDTH 240
#define PROG_ICON_HEIGHT 240
static GdkPixbuf *prog_icon_pixbuf;

/**
 * create_menu_bar - create a default menu bar to add to a window
 * @grid: the grid object to which the menu bar should be attached
 */
void
create_menu_bar(GtkWidget *grid)
{
	GtkWidget *menu_bar;
	GtkWidget *file_menu;
	GtkWidget *item_file;
	GtkWidget *item_file_quit;

	menu_bar = gtk_menu_bar_new();
	file_menu = gtk_menu_new();
	item_file = gtk_menu_item_new_with_label("File");
	item_file_quit = gtk_menu_item_new_with_label("Quit");

	gtk_menu_item_set_submenu(GTK_MENU_ITEM(item_file), file_menu);
	gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), item_file_quit);

	gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), item_file);

	gtk_grid_attach(GTK_GRID(grid), menu_bar, 0, 0, 1, 1);

	return;
}

void
create_window(void)
{
	window = gtk_application_window_new(app);
	gtk_window_set_default_size(GTK_WINDOW(window), WIN_DEFAULT_WIDTH, WIN_DEFAULT_HEIGHT);
	gtk_window_set_title(GTK_WINDOW(window), PROG_NAME " v" PROG_BUILD);
	gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);

	GError *error = NULL;

	prog_icon_pixbuf = gdk_pixbuf_new_from_file_at_size(
		PROG_ICON,
		PROG_ICON_WIDTH,
		PROG_ICON_HEIGHT,
		&error);

	if (error)
	{
		std::cerr << "create_window: failed to create pixbuf for application icon" << std::endl;
		g_error_free(error);
	}

	grid = gtk_grid_new();
	create_menu_bar(grid);

	toolbar = gtk_toolbar_new();
	tool_item_url = gtk_tool_item_new();
	item_button_get = gtk_tool_item_new();
	item_button_open = gtk_tool_item_new();

	entry_url = gtk_entry_new();
	gtk_container_add(GTK_CONTAINER(tool_item_url), entry_url);

	button_get = gtk_button_new_with_label("Get");
	button_open = gtk_button_new_with_label("Open");

	gtk_container_add(GTK_CONTAINER(item_button_get), button_get);
	gtk_container_add(GTK_CONTAINER(item_button_open), button_open);

	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), tool_item_url, 0);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item_button_get, 1);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item_button_open, 2);

	gtk_grid_attach(GTK_GRID(grid), toolbar, 0, 0, 1, 1);

	gtk_container_add(GTK_CONTAINER(window), grid);

	const gchar *authors[] = PROG_AUTHORS;

	gtk_show_about_dialog(
		GTK_WINDOW(window),
		"title", "About " PROG_NAME,
		"logo", prog_icon_pixbuf,
		"program-name", PROG_NAME,
		"comments", PROG_COMMENTS,
		"website", PROG_WEBSITE,
		"copyright", PROG_LICENCE,
		"authors", authors,
		NULL);

	gtk_widget_show_all(window);

	return;
}

int
main(int argc, char *argv[])
{
	app = gtk_application_new(PROG_NAME_DBUS, G_APPLICATION_FLAGS_NONE);
	g_signal_connect(app, "activate", G_CALLBACK(create_window), NULL);
	g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
}
