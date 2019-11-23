#include <list>
#include <map>
#include <gtk/gtk.h>

#define PROG_NAME "WikiGrab"
#define PROG_NAME_DBUS "org.wikigrab"
#define PROG_BUILD "0.0.5"
#define PROG_AUTHORS { "Gary Hannah", (gchar *)NULL }
#define PROG_LICENCE "© Licenced under GNU Library GPLv2"
#define PROG_COMMENTS "Wikipedia Article Downloader"

static GtkApplication *app;
static GtkWidget *window;
static GtkWidget *grid;

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
	window = gtk_application_window_new();
	gtk_window_set_default_size(GTK_WINDOW(window), WIN_DEFAULT_WIDTH, WIN_DEFAULT_HEIGHT);
	gtk_window_set_title(GTK_WINDOW(window), PROG_NAME " v" PROG_BUILD);
	gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);

	grid = gtk_grid_new();
	create_menu_bar(grid);

	gtk_container_add(GTK_CONTAINER(window), grid);

	const gchar *authors[] = PROG_AUTHORS;

	gtk_show_about_dialog(
		GTK_WINDOW(window),
		"program-name", PROG_NAME,
		"title", "About " PROG_NAME,
		"copyright", PROG_LICENCE,
		"authors", authors,
		NULL);

	gtk_widget_show_all(window);

	return;
}

void
start_gui(void)
{
	app = gtk_application_new(PROG_NAME_DBUS, G_APPLICATION_FLAGS_NONE);
	g_signal_connect(app, "activate", G_CALLBACK(create_window), NULL);
	g_application_run(G_APPLICATION(app));
	g_object_unref(app);
}
