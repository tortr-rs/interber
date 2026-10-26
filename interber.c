/* interber - a web browser that looks like a tty: tabs, one line to type in, nothing else */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <webkit2/webkit2.h>

#define LENGTH(X)       (sizeof(X) / sizeof(X)[0])
#define NCLOSED         16

typedef struct {
	WebKitWebView *view;
	GtkWidget *label;
	int loading;
} Tab;

/* settings, from ~/.config/interber/config */
static char home[1024] = "interber:home", search[1024] = "https://duckduckgo.com/?q=%s";
static char downloads[1024], fg[32] = "grey", bg[32] = "black";
static int javascript = 1, remember = 1, zoom = 100;

static const char defaultconfig[] =
"# interber settings\n"
"# change what's after the = and save. it works on the next page you open\n"
"\n"
"# the first page, and new tabs. interber:home is your bookmarks\n"
"home page = interber:home\n"
"# where words you type go. %s is what you typed\n"
"search = https://duckduckgo.com/?q=%s\n"
"\n"
"# tabs you had open come back when you start it again\n"
"remember tabs = on\n"
"javascript = on\n"
"# how big pages are, in percent\n"
"zoom = 100%\n"
"# where downloads go. nothing means ~/Downloads\n"
"downloads =\n"
"\n"
"# colours: black blue green cyan red magenta brown grey\n"
"#          dark grey, light blue, light green, light cyan, light red, pink, yellow, white\n"
"#          or #rrggbb\n"
"text colour = grey\n"
"background colour = black\n"
"\n"
"# keys: ctrl+t new tab, ctrl+w close it, ctrl+shift+t bring it back, ctrl+tab next tab, ctrl+1 to 9 a tab,\n"
"# ctrl+l the address line, ctrl+r reload, alt+left back, alt+right forward, ctrl+f find, ctrl+d bookmark,\n"
"# ctrl+plus and ctrl+minus zoom, ctrl+0 back to normal, f11 full screen, ctrl+h home, ctrl+q quit.\n"
"# middle click or ctrl+click a link for a new tab\n";

static GtkWidget *win, *book, *tabline, *entry, *status, *bars[2];
static GtkListStore *known;	/* history and bookmarks, for the address line */
static GHashTable *seen;
static WebKitWebContext *ctx;
static WebKitSettings *settings;
static GtkCssProvider *css;
static char cfgdir[1024], datadir[1024], cachedir[1024], cfgpath[1100];
static char *closed[NCLOSED];
static int nclosed, findmode;
static char hover[1024], dlstatus[256];
static time_t cfgmtime;

static Tab *newtab(const char *uri, WebKitWebView *related, int show);

/* ---- small things ---- */

static char *
trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	for (e = s + strlen(s); e > s && isspace((unsigned char)e[-1]); e--);
	*e = '\0';
	return s;
}

/* "Dark  Grey" -> "darkgrey" */
static void
squash(char *dst, const char *src, size_t n)
{
	size_t i = 0;

	for (; *src && i < n - 1; src++)
		if (!isspace((unsigned char)*src))
			dst[i++] = tolower((unsigned char)*src);
	dst[i] = '\0';
}

static int
yes(const char *v)
{
	char s[16];

	squash(s, v, sizeof s);
	return !strcmp(s, "on") || !strcmp(s, "yes") || !strcmp(s, "true") || !strcmp(s, "1");
}

static const char *
colourhex(const char *name)
{
	static const char *vga[][2] = {
		{ "black", "#000000" }, { "blue", "#0000aa" }, { "green", "#00aa00" }, { "cyan", "#00aaaa" },
		{ "red", "#aa0000" }, { "magenta", "#aa00aa" }, { "brown", "#aa5500" }, { "grey", "#aaaaaa" },
		{ "gray", "#aaaaaa" }, { "darkgrey", "#555555" }, { "darkgray", "#555555" },
		{ "lightblue", "#5555ff" }, { "lightgreen", "#55ff55" }, { "lightcyan", "#55ffff" },
		{ "lightred", "#ff5555" }, { "pink", "#ff55ff" }, { "yellow", "#ffff55" }, { "white", "#ffffff" },
	};
	char s[32];
	unsigned int i;

	squash(s, name, sizeof s);
	for (i = 0; i < LENGTH(vga); i++)
		if (!strcmp(s, vga[i][0]))
			return vga[i][1];
	return name[0] == '#' ? name : "#aaaaaa";
}

/* the first n characters of s */
static void
ucut(char *dst, const char *s, int n, size_t size)
{
	size_t i = 0;

	for (; *s && i < size - 1; s++) {
		if ((*s & 0xc0) != 0x80 && n-- <= 0)
			break;
		dst[i++] = *s;
	}
	dst[i] = '\0';
}

static void
run(const char *cmd)
{
	GError *err = NULL;

	if (!g_spawn_command_line_async(cmd, &err))
		g_clear_error(&err);
}

/* ---- settings ---- */

static void
loadconfig(void)
{
	char line[1024], key[64], *name, *value, *eq;
	struct stat st;
	FILE *f;

	if (stat(cfgpath, &st) < 0 && (f = fopen(cfgpath, "w"))) {
		fputs(defaultconfig, f);
		fclose(f);
	}
	if (stat(cfgpath, &st) == 0)
		cfgmtime = st.st_mtime;
	downloads[0] = '\0';
	if (!(f = fopen(cfgpath, "r")))
		return;
	while (fgets(line, sizeof line, f)) {
		/* a # in a url isn't a comment: only at the start of a line */
		if (*trim(line) == '#' || !(eq = strchr(line, '=')))
			continue;
		*eq = '\0';
		name = trim(line);
		value = trim(eq + 1);
		squash(key, name, sizeof key);
		if (!strcmp(key, "homepage"))
			snprintf(home, sizeof home, "%s", value);
		else if (!strcmp(key, "search"))
			snprintf(search, sizeof search, "%s", value);
		else if (!strcmp(key, "remembertabs"))
			remember = yes(value);
		else if (!strcmp(key, "javascript"))
			javascript = yes(value);
		else if (!strcmp(key, "zoom"))
			zoom = atoi(value) > 0 ? atoi(value) : 100;
		else if (!strcmp(key, "downloads"))
			snprintf(downloads, sizeof downloads, "%s", value);
		else if (!strcmp(key, "textcolour") || !strcmp(key, "textcolor"))
			snprintf(fg, sizeof fg, "%s", value);
		else if (!strcmp(key, "backgroundcolour") || !strcmp(key, "backgroundcolor"))
			snprintf(bg, sizeof bg, "%s", value);
		else
			fprintf(stderr, "interber: %s: don't know '%s'\n", cfgpath, name);
	}
	fclose(f);
	if (!downloads[0])
		snprintf(downloads, sizeof downloads, "%s/Downloads", g_get_home_dir());
	else if (downloads[0] == '~') {
		snprintf(line, sizeof line, "%s%s", g_get_home_dir(), downloads + 1);
		snprintf(downloads, sizeof downloads, "%s", line);
	}
}

/* the console's font and colours, everywhere but the page */
static void
loadcss(void)
{
	char buf[2048];
	const char *f = colourhex(fg), *b = colourhex(bg);

	snprintf(buf, sizeof buf,
		"* { font-family: VGA, monospace; font-size: 16px; }\n"
		"window, box, entry, label, button, notebook, popover { background: %s; color: %s;"
		" border: none; border-radius: 0; box-shadow: none; outline: none; margin: 0;"
		" padding: 0; min-height: 0; background-image: none; text-shadow: none; }\n"
		"entry { caret-color: %s; padding: 0 8px; }\n"
		"entry selection, button.cur { background: %s; color: %s; }\n"
		"button { padding: 0 8px 0 0; }\n"
		"label.status { padding: 0 8px; }\n"
		"treeview, treeview.view { background: %s; color: %s; }\n"
		"treeview:selected { background: %s; color: %s; }\n",
		b, f, f, f, b, b, f, f, b);
	gtk_css_provider_load_from_data(css, buf, -1, NULL);
}

static void
applysettings(void)
{
	int i;

	webkit_settings_set_enable_javascript(settings, javascript);
	for (i = 0; i < gtk_notebook_get_n_pages(GTK_NOTEBOOK(book)); i++)
		webkit_web_view_set_zoom_level(WEBKIT_WEB_VIEW(gtk_notebook_get_nth_page(GTK_NOTEBOOK(book), i)), zoom / 100.0);
}

/* ---- history and bookmarks: plain text, one "url title" a line ---- */

static void
remember1(const char *uri, const char *title)
{
	GtkTreeIter it;

	if (!uri || !*uri || g_hash_table_contains(seen, uri) || !strncmp(uri, "interber:", 9))
		return;
	g_hash_table_add(seen, g_strdup(uri));
	gtk_list_store_append(known, &it);
	gtk_list_store_set(known, &it, 0, uri, 1, title ? title : "", -1);
}

static void
readlist(const char *path)
{
	char line[4096], *sp;
	FILE *f;

	if (!(f = fopen(path, "r")))
		return;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = '\0';
		if ((sp = strchr(line, ' ')))
			*sp++ = '\0';
		remember1(line, sp);
	}
	fclose(f);
}

static void
addline(const char *path, const char *uri, const char *title)
{
	FILE *f;

	if (!uri || !strncmp(uri, "interber:", 9) || !(f = fopen(path, "a")))
		return;
	fprintf(f, "%s %s\n", uri, title ? title : "");
	fclose(f);
}

/* the address line finds anything typed in the address or the title */
static gboolean
matchknown(GtkEntryCompletion *c, const char *key, GtkTreeIter *it, gpointer data)
{
	char *uri, *title, *k = g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(entry)), -1), *u, *t;
	gboolean ok;

	gtk_tree_model_get(GTK_TREE_MODEL(known), it, 0, &uri, 1, &title, -1);
	u = g_utf8_casefold(uri ? uri : "", -1);
	t = g_utf8_casefold(title ? title : "", -1);
	ok = *k && (strstr(u, k) || strstr(t, k));
	g_free(uri); g_free(title); g_free(u); g_free(t); g_free(k);
	return ok;
}

/* ---- the start page: your bookmarks ---- */

static void
homepage(WebKitURISchemeRequest *req, gpointer data)
{
	GString *s = g_string_new(NULL);
	char path[1100], line[4096], *sp, *e1, *e2;
	GInputStream *in;
	FILE *f;
	int n = 0;

	g_string_append_printf(s, "<!doctype html><meta charset=utf-8><title>home</title><style>"
		"body{background:%s;color:%s;font:16px VGA,monospace;margin:16px}"
		"a{color:%s;text-decoration:none}a:hover{background:%s;color:%s}"
		"form input{font:16px VGA,monospace;background:%s;color:%s;border:0;width:60ch;outline:0}"
		"</style><pre>Chamesle Linux interber\n\n<form action=interber:search>search: <input name=q autofocus></form>\nbookmarks:\n",
		colourhex(bg), colourhex(fg), colourhex(fg), colourhex(fg), colourhex(bg), colourhex(bg), colourhex(fg));
	snprintf(path, sizeof path, "%s/bookmarks", cfgdir);
	if ((f = fopen(path, "r"))) {
		while (fgets(line, sizeof line, f)) {
			line[strcspn(line, "\n")] = '\0';
			if (!*line)
				continue;
			if ((sp = strchr(line, ' ')))
				*sp++ = '\0';
			e1 = g_markup_escape_text(line, -1);
			e2 = g_markup_escape_text(sp && *sp ? sp : line, -1);
			g_string_append_printf(s, "  <a href=\"%s\">%s</a>\n", e1, e2);
			g_free(e1); g_free(e2);
			n++;
		}
		fclose(f);
	}
	if (!n)
		g_string_append(s, "  none yet: ctrl+d keeps the page you're on\n");
	g_string_append(s, "</pre>");
	in = g_memory_input_stream_new_from_data(g_string_free(s, FALSE), -1, g_free);
	webkit_uri_scheme_request_finish(req, in, -1, "text/html");
	g_object_unref(in);
}

/* ---- tabs ---- */

static Tab *
tabof(WebKitWebView *v)
{
	return g_object_get_data(G_OBJECT(v), "tab");
}

static Tab *
curtab(void)
{
	int i = gtk_notebook_get_current_page(GTK_NOTEBOOK(book));
	GtkWidget *w = i >= 0 ? gtk_notebook_get_nth_page(GTK_NOTEBOOK(book), i) : NULL;

	return w ? tabof(WEBKIT_WEB_VIEW(w)) : NULL;
}

static void
updatestatus(void)
{
	char buf[1300];
	Tab *t = curtab();

	if (dlstatus[0])
		snprintf(buf, sizeof buf, "%s", dlstatus);
	else if (hover[0])
		ucut(buf, hover, 60, sizeof buf);
	else if (t && t->loading)
		snprintf(buf, sizeof buf, "%d%%", (int)(webkit_web_view_get_estimated_load_progress(t->view) * 100));
	else
		buf[0] = '\0';
	gtk_label_set_text(GTK_LABEL(status), buf);
}

/* the address line shows where you are, unless you're typing in it */
static void
showuri(void)
{
	Tab *t = curtab();
	const char *u = t ? webkit_web_view_get_uri(t->view) : NULL;

	if (!gtk_widget_has_focus(entry) && !findmode)
		gtk_entry_set_text(GTK_ENTRY(entry), u && strncmp(u, "interber:", 9) ? u : "");
}

/* [ github ] [ youtube ] on top, only when there's more than one */
static void
drawtabs(void)
{
	GtkNotebook *nb = GTK_NOTEBOOK(book);
	int i, n = gtk_notebook_get_n_pages(nb), cur = gtk_notebook_get_current_page(nb);
	char name[64], buf[96];
	const char *title;
	Tab *t;

	for (i = 0; i < n; i++) {
		t = tabof(WEBKIT_WEB_VIEW(gtk_notebook_get_nth_page(nb, i)));
		title = webkit_web_view_get_title(t->view);
		if (!title || !*title)
			title = webkit_web_view_get_uri(t->view);
		ucut(name, title && *title ? title : "new tab", 20, sizeof name);
		snprintf(buf, sizeof buf, "[ %s ]", name);
		gtk_button_set_label(GTK_BUTTON(t->label), buf);
		if (i == cur)
			gtk_style_context_add_class(gtk_widget_get_style_context(t->label), "cur");
		else
			gtk_style_context_remove_class(gtk_widget_get_style_context(t->label), "cur");
		gtk_box_reorder_child(GTK_BOX(tabline), t->label, i);
	}
	gtk_widget_set_visible(tabline, n > 1);
	t = curtab();
	title = t ? webkit_web_view_get_title(t->view) : NULL;
	gtk_window_set_title(GTK_WINDOW(win), title && *title ? title : "interber");
}

static void
go(Tab *t, const char *text)
{
	char *u, *q, buf[4096];
	const char *p;

	while (isspace((unsigned char)*text))
		text++;
	if (!*text)
		return;
	if (strstr(text, "://") || !strncmp(text, "about:", 6) || !strncmp(text, "interber:", 9)) {
		webkit_web_view_load_uri(t->view, text);
		return;
	}
	if (text[0] == '/' || text[0] == '~') {
		snprintf(buf, sizeof buf, "%s%s", text[0] == '~' ? g_get_home_dir() : "", text + (text[0] == '~'));
		u = g_filename_to_uri(buf, NULL, NULL);
		if (u)
			webkit_web_view_load_uri(t->view, u);
		g_free(u);
		return;
	}
	/* an address has a dot or a port and no spaces. anything else is a search */
	if (!strchr(text, ' ') && (strchr(text, '.') || strchr(text, ':') || !strcmp(text, "localhost"))) {
		snprintf(buf, sizeof buf, "https://%s", text);
		webkit_web_view_load_uri(t->view, buf);
		return;
	}
	q = g_uri_escape_string(text, NULL, TRUE);
	if ((p = strstr(search, "%s")))
		snprintf(buf, sizeof buf, "%.*s%s%s", (int)(p - search), search, q, p + 2);
	else
		snprintf(buf, sizeof buf, "%s%s", search, q);
	g_free(q);
	webkit_web_view_load_uri(t->view, buf);
}

static void
closetab(Tab *t)
{
	const char *u = webkit_web_view_get_uri(t->view);
	int i;

	if (u) {
		if (nclosed == NCLOSED) {
			g_free(closed[0]);
			memmove(closed, closed + 1, (NCLOSED - 1) * sizeof *closed);
			nclosed--;
		}
		closed[nclosed++] = g_strdup(u);
	}
	i = gtk_notebook_page_num(GTK_NOTEBOOK(book), GTK_WIDGET(t->view));
	gtk_widget_destroy(t->label);
	gtk_notebook_remove_page(GTK_NOTEBOOK(book), i);
	g_free(t);
	if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(book)) == 0) {
		gtk_main_quit();
		return;
	}
	drawtabs();
	showuri();
}

static void
savetabs(void)
{
	char path[1100];
	int i, n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(book));
	const char *u;
	FILE *f;

	snprintf(path, sizeof path, "%s/tabs", datadir);
	if (!remember) {
		unlink(path);
		return;
	}
	if (!(f = fopen(path, "w")))
		return;
	for (i = 0; i < n; i++)
		if ((u = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(gtk_notebook_get_nth_page(GTK_NOTEBOOK(book), i)))))
			fprintf(f, "%s\n", u);
	fclose(f);
}

/* ---- what pages do ---- */

static void
onchange(WebKitWebView *v, GParamSpec *ps, gpointer data)
{
	if (tabof(v) == curtab()) {
		showuri();
		updatestatus();
	}
	drawtabs();
}

static void
onload(WebKitWebView *v, WebKitLoadEvent e, gpointer data)
{
	Tab *t = tabof(v);
	char path[1100];

	t->loading = e != WEBKIT_LOAD_FINISHED;
	if (e == WEBKIT_LOAD_FINISHED) {
		snprintf(path, sizeof path, "%s/history", datadir);
		if (!g_hash_table_contains(seen, webkit_web_view_get_uri(v) ? webkit_web_view_get_uri(v) : ""))
			addline(path, webkit_web_view_get_uri(v), webkit_web_view_get_title(v));
		remember1(webkit_web_view_get_uri(v), webkit_web_view_get_title(v));
	}
	if (t == curtab())
		updatestatus();
}

static void
onhover(WebKitWebView *v, WebKitHitTestResult *h, guint mods, gpointer data)
{
	const char *u = webkit_hit_test_result_context_is_link(h) ? webkit_hit_test_result_get_link_uri(h) : NULL;

	snprintf(hover, sizeof hover, "%s", u ? u : "");
	updatestatus();
}

/* target=_blank and window.open: a new tab */
static GtkWidget *
oncreate(WebKitWebView *v, WebKitNavigationAction *a, gpointer data)
{
	return GTK_WIDGET(newtab(NULL, v, 1)->view);
}

static gboolean
onpolicy(WebKitWebView *v, WebKitPolicyDecision *d, WebKitPolicyDecisionType type, gpointer data)
{
	WebKitNavigationAction *a;
	WebKitResponsePolicyDecision *r;
	const char *u, *q;
	char *p, *t;

	if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
		a = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(d));
		u = webkit_uri_request_get_uri(webkit_navigation_action_get_request(a));
		/* the home page's search box */
		if (u && !strncmp(u, "interber:search", 15)) {
			webkit_policy_decision_ignore(d);
			if ((q = strstr(u, "q="))) {
				t = g_strdup(q + 2);
				for (p = t; *p; p++)
					if (*p == '+')
						*p = ' ';
				p = g_uri_unescape_string(t, NULL);
				go(tabof(v), p ? p : t);
				g_free(p);
				g_free(t);
			}
			return TRUE;
		}
		/* middle click or ctrl+click: in a new tab, behind this one */
		if (webkit_navigation_action_get_navigation_type(a) == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED
		&& (webkit_navigation_action_get_mouse_button(a) == 2 || (webkit_navigation_action_get_modifiers(a) & GDK_CONTROL_MASK))) {
			newtab(webkit_uri_request_get_uri(webkit_navigation_action_get_request(a)), NULL, 0);
			webkit_policy_decision_ignore(d);
			return TRUE;
		}
	} else if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
		r = WEBKIT_RESPONSE_POLICY_DECISION(d);
		/* what it can't show, it saves */
		if (!webkit_response_policy_decision_is_mime_type_supported(r)) {
			webkit_policy_decision_download(d);
			return TRUE;
		}
	}
	return FALSE;
}

static void
onclose(WebKitWebView *v, gpointer data)
{
	closetab(tabof(v));
}

/* full screen video and such: the lines go */
static gboolean
onfull(WebKitWebView *v, gpointer full)
{
	gtk_widget_set_visible(bars[0], !full && gtk_notebook_get_n_pages(GTK_NOTEBOOK(book)) > 1);
	gtk_widget_set_visible(bars[1], !full);
	if (full)
		gtk_window_fullscreen(GTK_WINDOW(win));
	else
		gtk_window_unfullscreen(GTK_WINDOW(win));
	return TRUE;
}

/* the mouse's side buttons: back and forward */
static gboolean
onbutton(GtkWidget *w, GdkEventButton *e, gpointer data)
{
	if (e->type != GDK_BUTTON_PRESS)
		return FALSE;
	if (e->button == 8)
		webkit_web_view_go_back(WEBKIT_WEB_VIEW(w));
	else if (e->button == 9)
		webkit_web_view_go_forward(WEBKIT_WEB_VIEW(w));
	else
		return FALSE;
	return TRUE;
}

static void
ontabclick(GtkButton *b, gpointer view)
{
	gtk_notebook_set_current_page(GTK_NOTEBOOK(book), gtk_notebook_page_num(GTK_NOTEBOOK(book), view));
}

static gboolean
ontabpress(GtkWidget *b, GdkEventButton *e, gpointer view)
{
	if (e->button != 2)
		return FALSE;
	closetab(tabof(view));
	return TRUE;
}

static Tab *
newtab(const char *uri, WebKitWebView *related, int show)
{
	Tab *t = g_new0(Tab, 1);
	int i;

	if (related)
		t->view = WEBKIT_WEB_VIEW(webkit_web_view_new_with_related_view(related));
	else
		t->view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", ctx, "settings", settings, NULL));
	g_object_set_data(G_OBJECT(t->view), "tab", t);
	webkit_web_view_set_zoom_level(t->view, zoom / 100.0);
	g_signal_connect(t->view, "notify::title", G_CALLBACK(onchange), NULL);
	g_signal_connect(t->view, "notify::uri", G_CALLBACK(onchange), NULL);
	g_signal_connect(t->view, "notify::estimated-load-progress", G_CALLBACK(onchange), NULL);
	g_signal_connect(t->view, "load-changed", G_CALLBACK(onload), NULL);
	g_signal_connect(t->view, "mouse-target-changed", G_CALLBACK(onhover), NULL);
	g_signal_connect(t->view, "create", G_CALLBACK(oncreate), NULL);
	g_signal_connect(t->view, "decide-policy", G_CALLBACK(onpolicy), NULL);
	g_signal_connect(t->view, "close", G_CALLBACK(onclose), NULL);
	g_signal_connect(t->view, "enter-fullscreen", G_CALLBACK(onfull), GINT_TO_POINTER(1));
	g_signal_connect(t->view, "leave-fullscreen", G_CALLBACK(onfull), GINT_TO_POINTER(0));
	g_signal_connect(t->view, "button-press-event", G_CALLBACK(onbutton), NULL);

	t->label = gtk_button_new_with_label("[ new tab ]");
	gtk_button_set_relief(GTK_BUTTON(t->label), GTK_RELIEF_NONE);
	gtk_widget_set_can_focus(t->label, FALSE);
	g_signal_connect(t->label, "clicked", G_CALLBACK(ontabclick), t->view);
	g_signal_connect(t->label, "button-press-event", G_CALLBACK(ontabpress), t->view);
	gtk_box_pack_start(GTK_BOX(tabline), t->label, FALSE, FALSE, 0);
	gtk_widget_show(t->label);

	/* next to the one you're on, like every browser */
	i = gtk_notebook_get_current_page(GTK_NOTEBOOK(book));
	gtk_widget_show(GTK_WIDGET(t->view));
	i = gtk_notebook_insert_page(GTK_NOTEBOOK(book), GTK_WIDGET(t->view), NULL, i + 1);
	if (show)
		gtk_notebook_set_current_page(GTK_NOTEBOOK(book), i);
	if (uri)
		go(t, uri);
	drawtabs();
	return t;
}

/* ---- the address line ---- */

static void
onactivate(GtkEntry *e, gpointer data)
{
	const char *text = gtk_entry_get_text(e);
	Tab *t = curtab();
	WebKitFindController *fc;

	if (!t)
		return;
	if (findmode) {
		fc = webkit_web_view_get_find_controller(t->view);
		if (webkit_find_controller_get_search_text(fc) && !strcmp(webkit_find_controller_get_search_text(fc), text))
			webkit_find_controller_search_next(fc);
		else
			webkit_find_controller_search(fc, text, WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND, G_MAXUINT);
		return;
	}
	go(t, text);
	gtk_widget_grab_focus(GTK_WIDGET(t->view));
}

static void
stopfind(void)
{
	Tab *t = curtab();

	if (!findmode)
		return;
	findmode = 0;
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), NULL);
	if (t)
		webkit_find_controller_search_finish(webkit_web_view_get_find_controller(t->view));
}

static gboolean
redrawtabs(gpointer data)
{
	drawtabs();
	return G_SOURCE_REMOVE;
}

static void
onswitch(GtkNotebook *nb, GtkWidget *page, guint n, gpointer data)
{
	stopfind();
	hover[0] = '\0';
	/* the page isn't current yet while this runs */
	g_idle_add(redrawtabs, NULL);
	gtk_entry_set_text(GTK_ENTRY(entry), "");
	if (webkit_web_view_get_uri(WEBKIT_WEB_VIEW(page)) && strncmp(webkit_web_view_get_uri(WEBKIT_WEB_VIEW(page)), "interber:", 9))
		gtk_entry_set_text(GTK_ENTRY(entry), webkit_web_view_get_uri(WEBKIT_WEB_VIEW(page)));
	gtk_widget_grab_focus(page);
}

/* ---- downloads: straight into ~/Downloads ---- */

static gboolean
ondest(WebKitDownload *d, char *name, gpointer data)
{
	char path[2048], base[1024], *dot, *uri;
	int i;

	g_mkdir_with_parents(downloads, 0755);
	snprintf(base, sizeof base, "%s", name && *name ? name : "download");
	snprintf(path, sizeof path, "%s/%s", downloads, base);
	/* never over something that's there: name (1).ext */
	for (i = 1; access(path, F_OK) == 0 && i < 1000; i++) {
		dot = strrchr(base, '.');
		if (dot && dot != base)
			snprintf(path, sizeof path, "%s/%.*s (%d)%s", downloads, (int)(dot - base), base, i, dot);
		else
			snprintf(path, sizeof path, "%s/%s (%d)", downloads, base, i);
	}
	uri = g_filename_to_uri(path, NULL, NULL);
	webkit_download_set_destination(d, uri);
	g_free(uri);
	return TRUE;
}

static void
dlname(WebKitDownload *d, char *buf, size_t n)
{
	const char *dest = webkit_download_get_destination(d);
	char *path = dest ? g_filename_from_uri(dest, NULL, NULL) : NULL;
	char *base = path ? g_path_get_basename(path) : NULL;

	ucut(buf, base ? base : "download", 30, n);
	g_free(base);
	g_free(path);
}

static void
ondlprogress(WebKitDownload *d, GParamSpec *ps, gpointer data)
{
	char name[128];

	dlname(d, name, sizeof name);
	snprintf(dlstatus, sizeof dlstatus, "%s %d%%", name, (int)(webkit_download_get_estimated_progress(d) * 100));
	updatestatus();
}

static gboolean
cleardl(gpointer data)
{
	dlstatus[0] = '\0';
	updatestatus();
	return G_SOURCE_REMOVE;
}

static void
ondlfinished(WebKitDownload *d, gpointer data)
{
	char name[128], cmd[512], *q;

	dlname(d, name, sizeof name);
	snprintf(dlstatus, sizeof dlstatus, "%s saved", name);
	updatestatus();
	g_timeout_add_seconds(5, cleardl, NULL);
	q = g_shell_quote(name);
	snprintf(cmd, sizeof cmd, "notify-send interber %s' is in Downloads'", q);
	g_free(q);
	run(cmd);
}

static void
ondlfailed(WebKitDownload *d, GError *err, gpointer data)
{
	snprintf(dlstatus, sizeof dlstatus, "download failed: %s", err->message);
	updatestatus();
	g_timeout_add_seconds(5, cleardl, NULL);
}

static void
ondownload(WebKitWebContext *c, WebKitDownload *d, gpointer data)
{
	g_signal_connect(d, "decide-destination", G_CALLBACK(ondest), NULL);
	g_signal_connect(d, "notify::estimated-progress", G_CALLBACK(ondlprogress), NULL);
	g_signal_connect(d, "finished", G_CALLBACK(ondlfinished), NULL);
	g_signal_connect(d, "failed", G_CALLBACK(ondlfailed), NULL);
}

/* ---- keys ---- */

static void
switchto(int i)
{
	int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(book));

	gtk_notebook_set_current_page(GTK_NOTEBOOK(book), (i % n + n) % n);
}

static gboolean
onkey(GtkWidget *w, GdkEventKey *e, gpointer data)
{
	guint mods = e->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK);
	guint k = gdk_keyval_to_lower(e->keyval);
	int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(book));
	Tab *t = curtab();
	char path[1100];
	const char *u;

	if (!t)
		return FALSE;
	if (mods == GDK_CONTROL_MASK) {
		switch (k) {
		case GDK_KEY_t: newtab(home, NULL, 1); gtk_widget_grab_focus(entry); return TRUE;
		case GDK_KEY_w: closetab(t); return TRUE;
		case GDK_KEY_q: gtk_main_quit(); return TRUE;
		case GDK_KEY_l: gtk_widget_grab_focus(entry); return TRUE;
		case GDK_KEY_r: webkit_web_view_reload(t->view); return TRUE;
		case GDK_KEY_h: go(t, home); return TRUE;
		case GDK_KEY_Tab: case GDK_KEY_Page_Down: switchto(cur + 1); return TRUE;
		case GDK_KEY_Page_Up: switchto(cur - 1); return TRUE;
		case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add:
			webkit_web_view_set_zoom_level(t->view, webkit_web_view_get_zoom_level(t->view) + 0.1);
			return TRUE;
		case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
			webkit_web_view_set_zoom_level(t->view, webkit_web_view_get_zoom_level(t->view) - 0.1);
			return TRUE;
		case GDK_KEY_0: webkit_web_view_set_zoom_level(t->view, zoom / 100.0); return TRUE;
		case GDK_KEY_f:
			findmode = 1;
			gtk_entry_set_text(GTK_ENTRY(entry), "");
			gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "find on this page");
			gtk_widget_grab_focus(entry);
			return TRUE;
		case GDK_KEY_d:
			u = webkit_web_view_get_uri(t->view);
			if (u && strncmp(u, "interber:", 9)) {
				snprintf(path, sizeof path, "%s/bookmarks", cfgdir);
				addline(path, u, webkit_web_view_get_title(t->view));
				remember1(u, webkit_web_view_get_title(t->view));
				snprintf(dlstatus, sizeof dlstatus, "bookmarked");
				updatestatus();
				g_timeout_add_seconds(2, cleardl, NULL);
			}
			return TRUE;
		}
		if (k >= GDK_KEY_1 && k <= GDK_KEY_9) {
			/* 9 is the last, like everywhere */
			switchto(k == GDK_KEY_9 ? -1 : (int)(k - GDK_KEY_1));
			return TRUE;
		}
	} else if (mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
		switch (k) {
		case GDK_KEY_t:
			if (nclosed) {
				newtab(closed[--nclosed], NULL, 1);
				g_free(closed[nclosed]);
			}
			return TRUE;
		case GDK_KEY_ISO_Left_Tab: case GDK_KEY_Tab: switchto(cur - 1); return TRUE;
		case GDK_KEY_r: webkit_web_view_reload_bypass_cache(t->view); return TRUE;
		}
	} else if (mods == GDK_MOD1_MASK) {
		switch (k) {
		case GDK_KEY_Left: webkit_web_view_go_back(t->view); return TRUE;
		case GDK_KEY_Right: webkit_web_view_go_forward(t->view); return TRUE;
		case GDK_KEY_d: gtk_widget_grab_focus(entry); return TRUE;
		case GDK_KEY_Home: go(t, home); return TRUE;
		}
	} else if (!mods) {
		switch (k) {
		case GDK_KEY_F5: webkit_web_view_reload(t->view); return TRUE;
		case GDK_KEY_F6: gtk_widget_grab_focus(entry); return TRUE;
		case GDK_KEY_F11:
			if (gdk_window_get_state(gtk_widget_get_window(win)) & GDK_WINDOW_STATE_FULLSCREEN)
				gtk_window_unfullscreen(GTK_WINDOW(win));
			else
				gtk_window_fullscreen(GTK_WINDOW(win));
			return TRUE;
		case GDK_KEY_Escape:
			if (gtk_widget_has_focus(entry) || findmode) {
				stopfind();
				gtk_widget_grab_focus(GTK_WIDGET(t->view));
				showuri();
				return TRUE;
			}
			if (t->loading) {
				webkit_web_view_stop_loading(t->view);
				return TRUE;
			}
			break;
		}
	}
	return FALSE;
}

/* the settings file changed: from the next page on */
static gboolean
checkconfig(gpointer data)
{
	struct stat st;

	if (stat(cfgpath, &st) == 0 && st.st_mtime != cfgmtime) {
		loadconfig();
		loadcss();
		applysettings();
	}
	return G_SOURCE_CONTINUE;
}

static void
onquit(GtkWidget *w, gpointer data)
{
	savetabs();
	gtk_main_quit();
}

static gboolean
ondelete(GtkWidget *w, GdkEvent *e, gpointer data)
{
	savetabs();
	return FALSE;
}

int
main(int argc, char *argv[])
{
	WebKitWebsiteDataManager *dm;
	WebKitCookieManager *cm;
	GtkEntryCompletion *comp;
	GtkCellRenderer *cell;
	GtkWidget *box, *bottom;
	char path[1100], line[4096];
	FILE *f;
	int i, opened = 0;

	if (argc == 2 && !strcmp(argv[1], "-v")) {
		puts("interber-" VERSION);
		return 0;
	}
	g_set_prgname("interber");
	gtk_init(&argc, &argv);
	snprintf(cfgdir, sizeof cfgdir, "%s/interber", g_get_user_config_dir());
	snprintf(datadir, sizeof datadir, "%s/interber", g_get_user_data_dir());
	snprintf(cachedir, sizeof cachedir, "%s/interber", g_get_user_cache_dir());
	g_mkdir_with_parents(cfgdir, 0755);
	g_mkdir_with_parents(datadir, 0700);
	snprintf(cfgpath, sizeof cfgpath, "%s/config", cfgdir);
	loadconfig();

	/* logins stay: cookies in a file, the third party ones not at all */
	dm = webkit_website_data_manager_new("base-data-directory", datadir, "base-cache-directory", cachedir, NULL);
	webkit_website_data_manager_set_itp_enabled(dm, TRUE);
	ctx = webkit_web_context_new_with_website_data_manager(dm);
	cm = webkit_web_context_get_cookie_manager(ctx);
	snprintf(path, sizeof path, "%s/cookies.sqlite", datadir);
	webkit_cookie_manager_set_persistent_storage(cm, path, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
	webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
	webkit_web_context_register_uri_scheme(ctx, "interber", homepage, NULL, NULL);
	g_signal_connect(ctx, "download-started", G_CALLBACK(ondownload), NULL);
	settings = webkit_settings_new_with_settings("enable-developer-extras", TRUE, "enable-smooth-scrolling", TRUE,
	                                             "enable-back-forward-navigation-gestures", TRUE, NULL);

	css = gtk_css_provider_new();
	loadcss();
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
	                                          GTK_STYLE_PROVIDER_PRIORITY_USER);

	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(win), 1024, 768);
	g_signal_connect(win, "destroy", G_CALLBACK(onquit), NULL);
	g_signal_connect(win, "delete-event", G_CALLBACK(ondelete), NULL);
	g_signal_connect(win, "key-press-event", G_CALLBACK(onkey), NULL);
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(win), box);

	bars[0] = tabline = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_pack_start(GTK_BOX(box), tabline, FALSE, FALSE, 0);
	book = gtk_notebook_new();
	gtk_notebook_set_show_tabs(GTK_NOTEBOOK(book), FALSE);
	gtk_notebook_set_show_border(GTK_NOTEBOOK(book), FALSE);
	g_signal_connect(book, "switch-page", G_CALLBACK(onswitch), NULL);
	gtk_box_pack_start(GTK_BOX(box), book, TRUE, TRUE, 0);

	bars[1] = bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	entry = gtk_entry_new();
	gtk_entry_set_has_frame(GTK_ENTRY(entry), FALSE);
	g_signal_connect(entry, "activate", G_CALLBACK(onactivate), NULL);
	gtk_box_pack_start(GTK_BOX(bottom), entry, TRUE, TRUE, 0);
	status = gtk_label_new("");
	gtk_style_context_add_class(gtk_widget_get_style_context(status), "status");
	gtk_box_pack_end(GTK_BOX(bottom), status, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(box), bottom, FALSE, FALSE, 0);

	known = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	snprintf(path, sizeof path, "%s/bookmarks", cfgdir);
	readlist(path);
	snprintf(path, sizeof path, "%s/history", datadir);
	readlist(path);
	comp = gtk_entry_completion_new();
	gtk_entry_completion_set_model(comp, GTK_TREE_MODEL(known));
	gtk_entry_completion_set_text_column(comp, 0);
	gtk_entry_completion_set_match_func(comp, matchknown, NULL, NULL);
	gtk_entry_completion_set_minimum_key_length(comp, 2);
	cell = gtk_cell_renderer_text_new();
	gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(comp), cell, FALSE);
	gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(comp), cell, "text", 1);
	gtk_entry_set_completion(GTK_ENTRY(entry), comp);

	gtk_widget_show_all(win);
	/* interber URL..., or the tabs from last time, or home */
	for (i = 1; i < argc; i++, opened++)
		newtab(argv[i], NULL, opened == 0);
	snprintf(path, sizeof path, "%s/tabs", datadir);
	if (!opened && remember && (f = fopen(path, "r"))) {
		while (fgets(line, sizeof line, f)) {
			line[strcspn(line, "\n")] = '\0';
			if (*line)
				newtab(line, NULL, opened++ == 0);
		}
		fclose(f);
	}
	if (!opened)
		newtab(home, NULL, 1);
	switchto(0);
	drawtabs();
	g_timeout_add_seconds(2, checkconfig, NULL);
	gtk_main();
	return 0;
}
