// Scintilla source code edit control
// PlatGTK.cxx - implementation of platform facilities on GTK+/Linux
// Copyright 1998-2003 by Neil Hodgson <neilh@scintilla.org>
// The License.txt file describes the conditions under which this software may be distributed.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#define assert(c) ((c) ? (void)(0) : Platform::Assert(#c, __FILE__, __LINE__))

#include <glib.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include "Platform.h"

#include "Scintilla.h"
#include "ScintillaWidget.h"
#include "UniConversion.h"
#include "XPM.h"

/* GLIB must be compiled with thread support, otherwise we
   will bail on trying to use locks, and that could lead to
   problems for someone.  `glib-config --libs gthread` needs
   to be used to get the glib libraries for linking, otherwise
   g_thread_init will fail */
#define USE_LOCK defined(G_THREADS_ENABLED) && !defined(G_THREADS_IMPL_NONE)
/* Use fast way of getting char data on win32 to work around problems
   with gdk_string_extents. */
#define FAST_WAY

#ifdef G_OS_WIN32
#define snprintf _snprintf
#endif

#if GTK_MAJOR_VERSION >= 2
#define USE_PANGO 1
#endif

#ifdef _MSC_VER
// Ignore unreferenced local functions in GTK+ headers
#pragma warning(disable: 4505)
#endif

enum encodingType { singleByte, UTF8, dbcs};

struct LOGFONT {
	int size;
	bool bold;
	bool italic;
	int characterSet;
	char faceName[300];
};

#if USE_LOCK
static GMutex *fontMutex = NULL;

static void InitializeGLIBThreads() {
	if (!g_thread_supported()) {
		g_thread_init(NULL);
	}
}
#endif

static void FontMutexAllocate() {
#if USE_LOCK
	if (!fontMutex) {
		InitializeGLIBThreads();
		fontMutex = g_mutex_new();
	}
#endif
}

static void FontMutexFree() {
#if USE_LOCK
	if (fontMutex) {
		g_mutex_free(fontMutex);
		fontMutex = NULL;
	}
#endif
}

static void FontMutexLock() {
#if USE_LOCK
	g_mutex_lock(fontMutex);
#endif
}

static void FontMutexUnlock() {
#if USE_LOCK
	if (fontMutex) {
		g_mutex_unlock(fontMutex);
	}
#endif
}

// On GTK+ 1.x holds a GdkFont* but on GTK+ 2.x can hold a GdkFont* or a
// PangoFontDescription*.
class FontHandle {
	int width[128];
	encodingType et;
public:
	int ascent;
	GdkFont *pfont;
#ifdef USE_PANGO
	PangoFontDescription *pfd;
#endif
	FontHandle(GdkFont *pfont_) {
		et = singleByte;
		ascent = 0;
		pfont = pfont_;
#ifdef USE_PANGO
		pfd = 0;
#endif
		ResetWidths(et);
	}
#ifdef USE_PANGO
	FontHandle(PangoFontDescription *pfd_) {
		et = singleByte;
		ascent = 0;
		pfont = 0;
		pfd = pfd_;
		ResetWidths(et);
	}
#endif
	~FontHandle() {
		if (pfont)
			gdk_font_unref(pfont);
		pfont = 0;
#ifdef USE_PANGO
		if (pfd)
			pango_font_description_free(pfd);
		pfd = 0;
#endif
	}
	void ResetWidths(encodingType et_) {
		et = et_;
		for (int i=0; i<=127; i++) {
			width[i] = 0;
		}
	}
	int CharWidth(unsigned char ch, encodingType et_) {
		int w = 0;
		FontMutexLock();
		if ((ch <= 127) && (et == et_)) {
			w = width[ch];
		}
		FontMutexUnlock();
		return w;
	}
	void SetCharWidth(unsigned char ch, int w, encodingType et_) {
		if (ch <= 127) {
			FontMutexLock();
			if (et != et_) {
				ResetWidths(et_);
			}
			width[ch] = w;
			FontMutexUnlock();
		}
	}
};

// X has a 16 bit coordinate space, so stop drawing here to avoid wrapping
static const int maxCoordinate = 32000;

static FontHandle *PFont(Font &f) {
	return reinterpret_cast<FontHandle *>(f.GetID());
}

static GtkWidget *PWidget(WindowID id) {
	return reinterpret_cast<GtkWidget *>(id);
}

static GtkWidget *PWidget(Window &w) {
	return PWidget(w.GetID());
}

Point Point::FromLong(long lpoint) {
	return Point(
	           Platform::LowShortFromLong(lpoint),
	           Platform::HighShortFromLong(lpoint));
}

Palette::Palette() {
	used = 0;
	allowRealization = false;
	allocatedPalette = 0;
	allocatedLen = 0;
}

Palette::~Palette() {
	Release();
}

void Palette::Release() {
	used = 0;
	delete [](reinterpret_cast<GdkColor *>(allocatedPalette));
	allocatedPalette = 0;
	allocatedLen = 0;
}

// This method either adds a colour to the list of wanted colours (want==true)
// or retrieves the allocated colour back to the ColourPair.
// This is one method to make it easier to keep the code for wanting and retrieving in sync.
void Palette::WantFind(ColourPair &cp, bool want) {
	if (want) {
		for (int i = 0; i < used; i++) {
			if (entries[i].desired == cp.desired)
				return;
		}

		if (used < numEntries) {
			entries[used].desired = cp.desired;
			entries[used].allocated.Set(cp.desired.AsLong());
			used++;
		}
	} else {
		for (int i = 0; i < used; i++) {
			if (entries[i].desired == cp.desired) {
				cp.allocated = entries[i].allocated;
				return;
			}
		}
		cp.allocated.Set(cp.desired.AsLong());
	}
}

void Palette::Allocate(Window &w) {
	if (allocatedPalette) {
		gdk_colormap_free_colors(gtk_widget_get_colormap(PWidget(w)),
		                         reinterpret_cast<GdkColor *>(allocatedPalette),
		                         allocatedLen);
		delete [](reinterpret_cast<GdkColor *>(allocatedPalette));
		allocatedPalette = 0;
		allocatedLen = 0;
	}
	GdkColor *paletteNew = new GdkColor[used];
	allocatedPalette = paletteNew;
	gboolean *successPalette = new gboolean[used];
	if (paletteNew) {
		allocatedLen = used;
		int iPal = 0;
		for (iPal = 0; iPal < used; iPal++) {
			paletteNew[iPal].red = entries[iPal].desired.GetRed() * (65535 / 255);
			paletteNew[iPal].green = entries[iPal].desired.GetGreen() * (65535 / 255);
			paletteNew[iPal].blue = entries[iPal].desired.GetBlue() * (65535 / 255);
			paletteNew[iPal].pixel = entries[iPal].desired.AsLong();
		}
		gdk_colormap_alloc_colors(gtk_widget_get_colormap(PWidget(w)),
		                          paletteNew, allocatedLen, FALSE, TRUE,
		                          successPalette);
		for (iPal = 0; iPal < used; iPal++) {
			entries[iPal].allocated.Set(paletteNew[iPal].pixel);
		}
	}
	delete []successPalette;
}

static const char *CharacterSetName(int characterSet) {
	switch (characterSet) {
	case SC_CHARSET_ANSI:
		return "iso8859-*";
	case SC_CHARSET_DEFAULT:
		return "iso8859-*";
	case SC_CHARSET_BALTIC:
		return "*-*";
	case SC_CHARSET_CHINESEBIG5:
		return "*-*";
	case SC_CHARSET_EASTEUROPE:
		return "*-2";
	case SC_CHARSET_GB2312:
		return "gb2312.1980-*";
	case SC_CHARSET_GREEK:
		return "*-7";
	case SC_CHARSET_HANGUL:
		return "ksc5601.1987-*";
	case SC_CHARSET_MAC:
		return "*-*";
	case SC_CHARSET_OEM:
		return "*-*";
	case SC_CHARSET_RUSSIAN:
		return "*-r";
	case SC_CHARSET_SHIFTJIS:
		return "jisx0208.1983-*";
	case SC_CHARSET_SYMBOL:
		return "*-*";
	case SC_CHARSET_TURKISH:
		return "*-9";
	case SC_CHARSET_JOHAB:
		return "*-*";
	case SC_CHARSET_HEBREW:
		return "*-8";
	case SC_CHARSET_ARABIC:
		return "*-6";
	case SC_CHARSET_VIETNAMESE:
		return "*-*";
	case SC_CHARSET_THAI:
		return "*-1";
	default:
		return "*-*";
	}
}

static bool IsDBCSCharacterSet(int characterSet) {
	switch (characterSet) {
	case SC_CHARSET_GB2312:
	case SC_CHARSET_HANGUL:
	case SC_CHARSET_SHIFTJIS:
	case SC_CHARSET_CHINESEBIG5:
		return true;
	default:
		return false;
	}
}

static void GenerateFontSpecStrings(const char *fontName, int characterSet,
                                    char *foundary, int foundary_len,
                                    char *faceName, int faceName_len,
                                    char *charset, int charset_len) {
	// supported font strings include:
	// foundary-fontface-isoxxx-x
	// fontface-isoxxx-x
	// foundary-fontface
	// fontface
	if (strchr(fontName, '-')) {
		char tmp[300];
		char *d1 = NULL, *d2 = NULL, *d3 = NULL;
		strncpy(tmp, fontName, sizeof(tmp) - 1);
		d1 = strchr(tmp, '-');
		// we know the first dash exists
		d2 = strchr(d1 + 1, '-');
		if (d2)
			d3 = strchr(d2 + 1, '-');
		if (d3) {
			// foundary-fontface-isoxxx-x
			*d2 = '\0';
			foundary[0] = '-';
			foundary[1] = '\0';
			strncpy(faceName, tmp, foundary_len - 1);
			strncpy(charset, d2 + 1, charset_len - 1);
		} else if (d2) {
			// fontface-isoxxx-x
			*d1 = '\0';
			strcpy(foundary, "-*-");
			strncpy(faceName, tmp, faceName_len - 1);
			strncpy(charset, d1 + 1, charset_len - 1);
		} else {
			// foundary-fontface
			foundary[0] = '-';
			foundary[1] = '\0';
			strncpy(faceName, tmp, faceName_len - 1);
			strncpy(charset, CharacterSetName(characterSet), charset_len - 1);
		}
	} else {
		strncpy(foundary, "-*-", foundary_len);
		strncpy(faceName, fontName, faceName_len - 1);
		strncpy(charset, CharacterSetName(characterSet), charset_len - 1);
	}
}

static void SetLogFont(LOGFONT &lf, const char *faceName, int characterSet, int size, bool bold, bool italic) {
	memset(&lf, 0, sizeof(lf));
	lf.size = size;
	lf.bold = bold;
	lf.italic = italic;
	lf.characterSet = characterSet;
	strncpy(lf.faceName, faceName, sizeof(lf.faceName) - 1);
}

/**
 * Create a hash from the parameters for a font to allow easy checking for identity.
 * If one font is the same as another, its hash will be the same, but if the hash is the
 * same then they may still be different.
 */
static int HashFont(const char *faceName, int characterSet, int size, bool bold, bool italic) {
	return
	    size ^
	    (characterSet << 10) ^
	    (bold ? 0x10000000 : 0) ^
	    (italic ? 0x20000000 : 0) ^
	    faceName[0];
}

class FontCached : Font {
	FontCached *next;
	int usage;
	LOGFONT lf;
	int hash;
	FontCached(const char *faceName_, int characterSet_, int size_, bool bold_, bool italic_);
	~FontCached() {}
	bool SameAs(const char *faceName_, int characterSet_, int size_, bool bold_, bool italic_);
	virtual void Release();
	static FontID CreateNewFont(const char *fontName, int characterSet,
	                            int size, bool bold, bool italic);
	static FontCached *first;
public:
	static FontID FindOrCreate(const char *faceName_, int characterSet_, int size_, bool bold_, bool italic_);
	static void ReleaseId(FontID id_);
};

FontCached *FontCached::first = 0;

FontCached::FontCached(const char *faceName_, int characterSet_, int size_, bool bold_, bool italic_) :
next(0), usage(0), hash(0) {
	::SetLogFont(lf, faceName_, characterSet_, size_, bold_, italic_);
	hash = HashFont(faceName_, characterSet_, size_, bold_, italic_);
	id = CreateNewFont(faceName_, characterSet_, size_, bold_, italic_);
	usage = 1;
}

bool FontCached::SameAs(const char *faceName_, int characterSet_, int size_, bool bold_, bool italic_) {
	return
	    lf.size == size_ &&
	    lf.bold == bold_ &&
	    lf.italic == italic_ &&
	    lf.characterSet == characterSet_ &&
	    0 == strcmp(lf.faceName, faceName_);
}

void FontCached::Release() {
	if (id)
		delete PFont(*this);
	id = 0;
}

FontID FontCached::FindOrCreate(const char *faceName_, int characterSet_, int size_, bool bold_, bool italic_) {
	FontID ret = 0;
	FontMutexLock();
	int hashFind = HashFont(faceName_, characterSet_, size_, bold_, italic_);
	for (FontCached *cur = first; cur; cur = cur->next) {
		if ((cur->hash == hashFind) &&
		        cur->SameAs(faceName_, characterSet_, size_, bold_, italic_)) {
			cur->usage++;
			ret = cur->id;
		}
	}
	if (ret == 0) {
		FontCached *fc = new FontCached(faceName_, characterSet_, size_, bold_, italic_);
		if (fc) {
			fc->next = first;
			first = fc;
			ret = fc->id;
		}
	}
	FontMutexUnlock();
	return ret;
}

void FontCached::ReleaseId(FontID id_) {
	FontMutexLock();
	FontCached **pcur = &first;
	for (FontCached *cur = first; cur; cur = cur->next) {
		if (cur->id == id_) {
			cur->usage--;
			if (cur->usage == 0) {
				*pcur = cur->next;
				cur->Release();
				cur->next = 0;
				delete cur;
			}
			break;
		}
		pcur = &cur->next;
	}
	FontMutexUnlock();
}

static GdkFont *LoadFontOrSet(const char *fontspec, int characterSet) {
	if (IsDBCSCharacterSet(characterSet)) {
		return gdk_fontset_load(fontspec);
	} else {
		return gdk_font_load(fontspec);
	}
}

FontID FontCached::CreateNewFont(const char *fontName, int characterSet,
                                 int size, bool bold, bool italic) {
	char fontset[1024];
	char fontspec[300];
	char foundary[50];
	char faceName[100];
	char charset[50];
	fontset[0] = '\0';
	fontspec[0] = '\0';
	foundary[0] = '\0';
	faceName[0] = '\0';
	charset[0] = '\0';

#ifdef USE_PANGO
	if (fontName[0] == '!') {
		PangoFontDescription *pfd = pango_font_description_new();
		if (pfd) {
			pango_font_description_set_family(pfd, fontName+1);
			pango_font_description_set_size(pfd, size * PANGO_SCALE);
			pango_font_description_set_weight(pfd, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
			pango_font_description_set_style(pfd, italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
			return new FontHandle(pfd);
		}
	}
#endif

	GdkFont *newid = 0;
	// If name of the font begins with a '-', assume, that it is
	// a full fontspec.
	if (fontName[0] == '-') {
		if (strchr(fontName, ',') || IsDBCSCharacterSet(characterSet)) {
			newid = gdk_fontset_load(fontName);
		} else {
			newid = gdk_font_load(fontName);
		}
		if (!newid) {
			// Font not available so substitute a reasonable code font
			// iso8859 appears to only allow western characters.
			newid = LoadFontOrSet("-*-*-*-*-*-*-*-*-*-*-*-*-iso8859-*",
				characterSet);
		}
		return new FontHandle(newid);
	}

	// it's not a full fontspec, build one.

	// This supports creating a FONT_SET
	// in a method that allows us to also set size, slant and
	// weight for the fontset.  The expected input is multiple
	// partial fontspecs seperated by comma
	// eg. adobe-courier-iso10646-1,*-courier-iso10646-1,*-*-*-*
	if (strchr(fontName, ',')) {
		// build a fontspec and use gdk_fontset_load
		int remaining = sizeof(fontset);
		char fontNameCopy[1024];
		strncpy(fontNameCopy, fontName, sizeof(fontNameCopy) - 1);
		char *fn = fontNameCopy;
		char *fp = strchr(fn, ',');
		for (;;) {
			const char *spec = "%s%s%s%s-*-*-*-%0d-*-*-*-*-%s";
			if (fontset[0] != '\0') {
				// if this is not the first font in the list,
				// append a comma seperator
				spec = ",%s%s%s%s-*-*-*-%0d-*-*-*-*-%s";
			}

			if (fp)
				*fp = '\0'; // nullify the comma
			GenerateFontSpecStrings(fn, characterSet,
			                        foundary, sizeof(foundary),
			                        faceName, sizeof(faceName),
			                        charset, sizeof(charset));

			snprintf(fontspec,
			         sizeof(fontspec) - 1,
			         spec,
			         foundary, faceName,
			         bold ? "-bold" : "-medium",
			         italic ? "-i" : "-r",
			         size * 10,
			         charset);

			// if this is the first font in the list, and
			// we are doing italic, add an oblique font
			// to the list
			if (italic && fontset[0] == '\0') {
				strncat(fontset, fontspec, remaining - 1);
				remaining -= strlen(fontset);

				snprintf(fontspec,
				         sizeof(fontspec) - 1,
				         ",%s%s%s-o-*-*-*-%0d-*-*-*-*-%s",
				         foundary, faceName,
				         bold ? "-bold" : "-medium",
				         size * 10,
				         charset);
			}

			strncat(fontset, fontspec, remaining - 1);
			remaining -= strlen(fontset);

			if (!fp)
				break;

			fn = fp + 1;
			fp = strchr(fn, ',');
		}

		newid = gdk_fontset_load(fontset);
		if (newid)
			return new FontHandle(newid);

		// if fontset load failed, fall through, we'll use
		// the last font entry and continue to try and
		// get something that matches
	}

	// single fontspec support

	GenerateFontSpecStrings(fontName, characterSet,
	                        foundary, sizeof(foundary),
	                        faceName, sizeof(faceName),
	                        charset, sizeof(charset));

	snprintf(fontspec,
	         sizeof(fontspec) - 1,
	         "%s%s%s%s-*-*-*-%0d-*-*-*-*-%s",
	         foundary, faceName,
	         bold ? "-bold" : "-medium",
	         italic ? "-i" : "-r",
	         size * 10,
	         charset);
	newid = LoadFontOrSet(fontspec, characterSet);
	if (!newid) {
		// some fonts have oblique, not italic
		snprintf(fontspec,
		         sizeof(fontspec) - 1,
		         "%s%s%s%s-*-*-*-%0d-*-*-*-*-%s",
		         foundary, faceName,
		         bold ? "-bold" : "-medium",
		         italic ? "-o" : "-r",
		         size * 10,
		         charset);
		newid = LoadFontOrSet(fontspec, characterSet);
	}
	if (!newid) {
		snprintf(fontspec,
		         sizeof(fontspec) - 1,
		         "-*-*-*-*-*-*-*-%0d-*-*-*-*-%s",
		         size * 10,
		         charset);
		newid = gdk_font_load(fontspec);
	}
	if (!newid) {
		// Font not available so substitute a reasonable code font
		// iso8859 appears to only allow western characters.
		newid = LoadFontOrSet("-*-*-*-*-*-*-*-*-*-*-*-*-iso8859-*",
			characterSet);
	}
	return new FontHandle(newid);
}

Font::Font() : id(0) {}

Font::~Font() {}

void Font::Create(const char *faceName, int characterSet, int size, bool bold, bool italic) {
	Release();
	id = FontCached::FindOrCreate(faceName, characterSet, size, bold, italic);
}

void Font::Release() {
	if (id)
		FontCached::ReleaseId(id);
	id = 0;
}

class SurfaceImpl : public Surface {
	encodingType et;
	GdkDrawable *drawable;
	GdkGC *gc;
	GdkPixmap *ppixmap;
	int x;
	int y;
	bool inited;
	bool createdGC;
#ifdef USE_PANGO
	PangoContext *pcontext;
	PangoLayout *layout;
#endif
public:
	SurfaceImpl();
	virtual ~SurfaceImpl();

	void Init(WindowID wid);
	void Init(SurfaceID sid, WindowID wid);
	void InitPixMap(int width, int height, Surface *surface_, WindowID wid);

	void Release();
	bool Initialised();
	void PenColour(ColourAllocated fore);
	int LogPixelsY();
	int DeviceHeightFont(int points);
	void MoveTo(int x_, int y_);
	void LineTo(int x_, int y_);
	void Polygon(Point *pts, int npts, ColourAllocated fore, ColourAllocated back);
	void RectangleDraw(PRectangle rc, ColourAllocated fore, ColourAllocated back);
	void FillRectangle(PRectangle rc, ColourAllocated back);
	void FillRectangle(PRectangle rc, Surface &surfacePattern);
	void RoundedRectangle(PRectangle rc, ColourAllocated fore, ColourAllocated back);
	void Ellipse(PRectangle rc, ColourAllocated fore, ColourAllocated back);
	void Copy(PRectangle rc, Point from, Surface &surfaceSource);

	void DrawTextBase(PRectangle rc, Font &font_, int ybase, const char *s, int len, ColourAllocated fore);
	void DrawTextNoClip(PRectangle rc, Font &font_, int ybase, const char *s, int len, ColourAllocated fore, ColourAllocated back);
	void DrawTextClipped(PRectangle rc, Font &font_, int ybase, const char *s, int len, ColourAllocated fore, ColourAllocated back);
	void DrawTextTransparent(PRectangle rc, Font &font_, int ybase, const char *s, int len, ColourAllocated fore);
	void MeasureWidths(Font &font_, const char *s, int len, int *positions);
	int WidthText(Font &font_, const char *s, int len);
	int WidthChar(Font &font_, char ch);
	int Ascent(Font &font_);
	int Descent(Font &font_);
	int InternalLeading(Font &font_);
	int ExternalLeading(Font &font_);
	int Height(Font &font_);
	int AverageCharWidth(Font &font_);

	int SetPalette(Palette *pal, bool inBackGround);
	void SetClip(PRectangle rc);
	void FlushCachedState();

	void SetUnicodeMode(bool unicodeMode_);
	void SetDBCSMode(int codePage);
};

SurfaceImpl::SurfaceImpl() : et(singleByte), drawable(0), gc(0), ppixmap(0),
x(0), y(0), inited(false), createdGC(false)
#ifdef USE_PANGO
, pcontext(0), layout(0)
#endif
{
}

SurfaceImpl::~SurfaceImpl() {
	Release();
}

void SurfaceImpl::Release() {
	drawable = 0;
	if (createdGC) {
		createdGC = false;
		gdk_gc_unref(gc);
	}
	gc = 0;
	if (ppixmap)
		gdk_pixmap_unref(ppixmap);
	ppixmap = 0;
#ifdef USE_PANGO
	if (layout)
		g_object_unref(layout);
	layout = 0;
	if (pcontext)
		g_object_unref(pcontext);
	pcontext = 0;
#endif
	x = 0;
	y = 0;
	inited = false;
	createdGC = false;
}

bool SurfaceImpl::Initialised() {
	return inited;
}

// The WindowID argument is only used for Pango builds
#ifdef USE_PANGO
#define WID_NAME wid
#else
#define WID_NAME
#endif

void SurfaceImpl::Init(WindowID WID_NAME) {
	Release();
#ifdef USE_PANGO
	PLATFORM_ASSERT(wid);
	pcontext = gtk_widget_create_pango_context(PWidget(wid));
	PLATFORM_ASSERT(pcontext);
	layout = pango_layout_new(pcontext);
	PLATFORM_ASSERT(layout);
#endif
	inited = true;
}

void SurfaceImpl::Init(SurfaceID sid, WindowID WID_NAME) {
	PLATFORM_ASSERT(sid);
	GdkDrawable *drawable_ = reinterpret_cast<GdkDrawable *>(sid);
	Release();
#ifdef USE_PANGO
	PLATFORM_ASSERT(wid);
	pcontext = gtk_widget_create_pango_context(PWidget(wid));
	layout = pango_layout_new(pcontext);
#endif
	drawable = drawable_;
	gc = gdk_gc_new(drawable_);
	createdGC = true;
	inited = true;
}

void SurfaceImpl::InitPixMap(int width, int height, Surface *surface_, WindowID WID_NAME) {
	PLATFORM_ASSERT(surface_);
	Release();
	SurfaceImpl *surfImpl = static_cast<SurfaceImpl *>(surface_);
	PLATFORM_ASSERT(surfImpl->drawable);
#ifdef USE_PANGO
	PLATFORM_ASSERT(wid);
	pcontext = gtk_widget_create_pango_context(PWidget(wid));
	PLATFORM_ASSERT(pcontext);
	layout = pango_layout_new(pcontext);
	PLATFORM_ASSERT(layout);
#endif
	if (height > 0 && width > 0)
		ppixmap = gdk_pixmap_new(surfImpl->drawable, width, height, -1);
	drawable = ppixmap;
	gc = gdk_gc_new(surfImpl->drawable);
	createdGC = true;
	inited = true;
}

void SurfaceImpl::PenColour(ColourAllocated fore) {
	if (gc) {
		GdkColor co;
		co.pixel = fore.AsLong();
		gdk_gc_set_foreground(gc, &co);
	}
}

int SurfaceImpl::LogPixelsY() {
	return 72;
}

int SurfaceImpl::DeviceHeightFont(int points) {
	int logPix = LogPixelsY();
	return (points * logPix + logPix / 2) / 72;
}

void SurfaceImpl::MoveTo(int x_, int y_) {
	x = x_;
	y = y_;
}

void SurfaceImpl::LineTo(int x_, int y_) {
	if (drawable && gc) {
		gdk_draw_line(drawable, gc,
		              x, y,
		              x_, y_);
	}
	x = x_;
	y = y_;
}

void SurfaceImpl::Polygon(Point *pts, int npts, ColourAllocated fore,
                          ColourAllocated back) {
	GdkPoint gpts[20];
	if (npts < static_cast<int>((sizeof(gpts) / sizeof(gpts[0])))) {
		for (int i = 0;i < npts;i++) {
			gpts[i].x = pts[i].x;
			gpts[i].y = pts[i].y;
		}
		PenColour(back);
		gdk_draw_polygon(drawable, gc, 1, gpts, npts);
		PenColour(fore);
		gdk_draw_polygon(drawable, gc, 0, gpts, npts);
	}
}

void SurfaceImpl::RectangleDraw(PRectangle rc, ColourAllocated fore, ColourAllocated back) {
	if (gc && drawable) {
		PenColour(back);
		gdk_draw_rectangle(drawable, gc, 1,
		                   rc.left + 1, rc.top + 1,
		                   rc.right - rc.left - 2, rc.bottom - rc.top - 2);

		PenColour(fore);
		// The subtraction of 1 off the width and height here shouldn't be needed but
		// otherwise a different rectangle is drawn than would be done if the fill parameter == 1
		gdk_draw_rectangle(drawable, gc, 0,
		                   rc.left, rc.top,
		                   rc.right - rc.left - 1, rc.bottom - rc.top - 1);
	}
}

void SurfaceImpl::FillRectangle(PRectangle rc, ColourAllocated back) {
	PenColour(back);
	if (drawable && (rc.left < maxCoordinate)) {	// Protect against out of range
		gdk_draw_rectangle(drawable, gc, 1,
		                   rc.left, rc.top,
		                   rc.right - rc.left, rc.bottom - rc.top);
	}
}

void SurfaceImpl::FillRectangle(PRectangle rc, Surface &surfacePattern) {
	if (static_cast<SurfaceImpl &>(surfacePattern).drawable) {
		// Tile pattern over rectangle
		// Currently assumes 8x8 pattern
		int widthPat = 8;
		int heightPat = 8;
		for (int xTile = rc.left; xTile < rc.right; xTile += widthPat) {
			int widthx = (xTile + widthPat > rc.right) ? rc.right - xTile : widthPat;
			for (int yTile = rc.top; yTile < rc.bottom; yTile += heightPat) {
				int heighty = (yTile + heightPat > rc.bottom) ? rc.bottom - yTile : heightPat;
				gdk_draw_pixmap(drawable,
				                gc,
				                static_cast<SurfaceImpl &>(surfacePattern).drawable,
				                0, 0,
				                xTile, yTile,
				                widthx, heighty);
			}
		}
	} else {
		// Something is wrong so try to show anyway
		// Shows up black because colour not allocated
		FillRectangle(rc, ColourAllocated(0));
	}
}

void SurfaceImpl::RoundedRectangle(PRectangle rc, ColourAllocated fore, ColourAllocated back) {
	if (((rc.right - rc.left) > 4) && ((rc.bottom - rc.top) > 4)) {
		// Approximate a round rect with some cut off corners
		Point pts[] = {
		                  Point(rc.left + 2, rc.top),
		                  Point(rc.right - 2, rc.top),
		                  Point(rc.right, rc.top + 2),
		                  Point(rc.right, rc.bottom - 2),
		                  Point(rc.right - 2, rc.bottom),
		                  Point(rc.left + 2, rc.bottom),
		                  Point(rc.left, rc.bottom - 2),
		                  Point(rc.left, rc.top + 2),
		              };
		Polygon(pts, sizeof(pts) / sizeof(pts[0]), fore, back);
	} else {
		RectangleDraw(rc, fore, back);
	}
}

void SurfaceImpl::Ellipse(PRectangle rc, ColourAllocated fore, ColourAllocated back) {
	PenColour(back);
	gdk_draw_arc(drawable, gc, 1,
	             rc.left + 1, rc.top + 1,
	             rc.right - rc.left - 2, rc.bottom - rc.top - 2,
	             0, 32767);

	// The subtraction of 1 here is similar to the case for RectangleDraw
	PenColour(fore);
	gdk_draw_arc(drawable, gc, 0,
	             rc.left, rc.top,
	             rc.right - rc.left - 1, rc.bottom - rc.top - 1,
	             0, 32767);
}

void SurfaceImpl::Copy(PRectangle rc, Point from, Surface &surfaceSource) {
	if (static_cast<SurfaceImpl &>(surfaceSource).drawable) {
		gdk_draw_pixmap(drawable,
		                gc,
		                static_cast<SurfaceImpl &>(surfaceSource).drawable,
		                from.x, from.y,
		                rc.left, rc.top,
		                rc.right - rc.left, rc.bottom - rc.top);
	}
}

static size_t UTF8Len(char ch) {
	unsigned char uch = static_cast<unsigned char>(ch);
	if (uch < 0x80)
		return 1;
	else if (uch < (0x80 + 0x40 + 0x20))
		return 2;
	else
		return 3;
}

#ifdef USE_PANGO

static char *UTF8FromLatin1(const char *s, int len) {
	char *utfForm = new char[len*2+1];
	size_t lenU = 0;
	for (int i=0;i<len;i++) {
		unsigned int uch = static_cast<unsigned char>(s[i]);
		if (uch < 0x80) {
			utfForm[lenU++] = uch;
		} else {
			utfForm[lenU++] = static_cast<char>(0xC0 | (uch >> 6));
			utfForm[lenU++] = static_cast<char>(0x80 | (uch & 0x3f));
		}
	}
	utfForm[lenU] = '\0';
	return utfForm;
}

static char *UTF8FromGdkWChar(GdkWChar *wctext, int wclen) {
	char *utfForm = new char[wclen*3+1];	// Maximum of 3 UTF-8 bytes per character
	size_t lenU = 0;
	for (int i = 0; i < wclen && wctext[i]; i++) {
		unsigned int uch = wctext[i];
		if (uch < 0x80) {
			utfForm[lenU++] = static_cast<char>(uch);
		} else if (uch < 0x800) {
			utfForm[lenU++] = static_cast<char>(0xC0 | (uch >> 6));
			utfForm[lenU++] = static_cast<char>(0x80 | (uch & 0x3f));
		} else {
			utfForm[lenU++] = static_cast<char>(0xE0 | (uch >> 12));
			utfForm[lenU++] = static_cast<char>(0x80 | ((uch >> 6) & 0x3f));
			utfForm[lenU++] = static_cast<char>(0x80 | (uch & 0x3f));
		}
	}
	utfForm[lenU] = '\0';
	return utfForm;
}

static char *UTF8FromDBCS(const char *s, int len) {
	GdkWChar *wctext = new GdkWChar[len + 1];
	GdkWChar *wcp = wctext;
	int wclen = gdk_mbstowcs(wcp, s, len);
	if (wclen < 1) {
		// In the annoying case when non-locale chars in the line.
		// e.g. latin1 chars in Japanese locale.
		delete []wctext;
		return 0;
	}

	char *utfForm = UTF8FromGdkWChar(wctext, wclen);
	delete []wctext;
	return utfForm;
}

#endif

// On GTK+, wchar_t is 4 bytes

const int maxLengthTextRun = 10000;

void SurfaceImpl::DrawTextBase(PRectangle rc, Font &font_, int ybase, const char *s, int len,
                                 ColourAllocated fore) {
	PenColour(fore);
	if (gc && drawable) {
		int x = rc.left;
#ifdef USE_PANGO
		if (PFont(font_)->pfd) {
			char *utfForm = 0;
			if (et == UTF8) {
				pango_layout_set_text(layout, s, len);
			} else {
				if (et == dbcs) {
					// Convert to utf8
					utfForm = UTF8FromDBCS(s, len);
				}
				if (!utfForm)	// Latin1 or DBCS failed so treat as Latin1
					utfForm = UTF8FromLatin1(s, len);
				pango_layout_set_text(layout, utfForm, strlen(utfForm));
			}
			pango_layout_set_font_description(layout, PFont(font_)->pfd);
			PangoLayoutLine *pll = pango_layout_get_line(layout,0);
			gdk_draw_layout_line(drawable, gc, x, ybase, pll);
			delete []utfForm;
			return;
		}
#endif
		// Draw text as a series of segments to avoid limitations in X servers
		const int segmentLength = 1000;
		bool draw8bit = true;
		if (et != singleByte) {
			GdkWChar wctext[maxLengthTextRun];
			int wclen;
			if (et == UTF8) {
				wclen = UCS2FromUTF8(s, len,
					reinterpret_cast<wchar_t *>(wctext), maxLengthTextRun - 1);
			} else {	// dbcs, so convert using current locale
				wclen = gdk_mbstowcs(
					wctext, s, maxLengthTextRun - 1);
			}
			if (wclen > 0) {
				draw8bit = false;
				wctext[wclen] = L'\0';
				GdkWChar *wcp = wctext;
				while ((wclen > 0) && (x < maxCoordinate)) {
					int lenDraw = Platform::Minimum(wclen, segmentLength);
					gdk_draw_text_wc(drawable, PFont(font_)->pfont, gc,
							 x, ybase, wcp, lenDraw);
					wclen -= lenDraw;
					wcp += lenDraw;
					if (wclen > 0) {	// Avoid next calculation if possible as may be expensive
						x += gdk_text_width_wc(PFont(font_)->pfont,
								       wcp, lenDraw);
					}
				}
			}
		}
		if (draw8bit) {
			while ((len > 0) && (x < maxCoordinate)) {
				int lenDraw = Platform::Minimum(len, segmentLength);
				gdk_draw_text(drawable, PFont(font_)->pfont, gc,
				              x, ybase, s, lenDraw);
				len -= lenDraw;
				s += lenDraw;
				if (len > 0) {	// Avoid next calculation if possible as may be expensive
					x += gdk_text_width(PFont(font_)->pfont, s, lenDraw);
				}
			}
		}
	}
}

void SurfaceImpl::DrawTextNoClip(PRectangle rc, Font &font_, int ybase, const char *s, int len,
                                 ColourAllocated fore, ColourAllocated back) {
	FillRectangle(rc, back);
	DrawTextBase(rc, font_, ybase, s, len, fore);
}

// On GTK+, exactly same as DrawTextNoClip
void SurfaceImpl::DrawTextClipped(PRectangle rc, Font &font_, int ybase, const char *s, int len,
                                  ColourAllocated fore, ColourAllocated back) {
	FillRectangle(rc, back);
	DrawTextBase(rc, font_, ybase, s, len, fore);
}

void SurfaceImpl::DrawTextTransparent(PRectangle rc, Font &font_, int ybase, const char *s, int len,
                                  ColourAllocated fore) {
	// Avoid drawing spaces in transparent mode
	for (int i=0;i<len;i++) {
		if (s[i] != ' ') {
			DrawTextBase(rc, font_, ybase, s, len, fore);
			return;
		}
	}
}

void SurfaceImpl::MeasureWidths(Font &font_, const char *s, int len, int *positions) {
	if (font_.GetID()) {
		int totalWidth = 0;
#ifdef USE_PANGO
		if (PFont(font_)->pfd) {
			if (len == 1) {
				int width = PFont(font_)->CharWidth(*s, et);
				if (width) {
					positions[0] = width;
					return;
				}
			}
			PangoRectangle pos;
			pango_layout_set_font_description(layout, PFont(font_)->pfd);
			if (et == UTF8) {
				// Simple and direct as UTF-8 is native Pango encoding
				pango_layout_set_text(layout, s, len);
				int i = 0;
				while (i < len) {
					pango_layout_index_to_pos(layout, i+1, &pos);
					positions[i++] = PANGO_PIXELS(pos.x);
				}
			} else {
				int wclen = 0;
				if (et == dbcs) {
					GdkWChar *wctext = new GdkWChar[len + 1];
					GdkWChar *wcp = wctext;
					wclen = gdk_mbstowcs(wcp, s, len);
					if (wclen >= 1 ) {
						// Convert to UTF-8 so can ask Pango for widths, then
						// Loop through UTF-8 and DBCS forms, taking account of different
						// character byte lengths.
						char *utfForm = UTF8FromGdkWChar(wctext, wclen);
						pango_layout_set_text(layout, utfForm, strlen(utfForm));
						int i = 0;
						int iU = 0;
						while (i < len) {
							pango_layout_index_to_pos(layout, iU+1, &pos);
							iU += UTF8Len(utfForm[iU]);
							size_t lenChar = mblen(s+i, MB_CUR_MAX);
							while (lenChar--) {
								positions[i++] = PANGO_PIXELS(pos.x);
							}
						}
						delete []utfForm;
					}
					delete []wctext;
				}
				if (wclen < 1 ) {
					// Either Latin1 or DBCS conversion failed so treat as Latin1.
					char *utfForm = UTF8FromLatin1(s, len);
					pango_layout_set_text(layout, utfForm, strlen(utfForm));
					int i = 0;
					int iU = 0;
					while (i < len) {
						pango_layout_index_to_pos(layout, iU+1, &pos);
						iU += UTF8Len(s[i]);
						positions[i++] = PANGO_PIXELS(pos.x);
					}
					delete []utfForm;
				}
			}
			if (len == 1) {
				PFont(font_)->SetCharWidth(*s, positions[0], et);
			}
			return;
		}
#endif
		GdkFont *gf = PFont(font_)->pfont;
		bool measure8bit = true;
		if (et != singleByte) {
			GdkWChar wctext[maxLengthTextRun];
			int wclen;
			if (et == UTF8) {
				wclen = UCS2FromUTF8(s, len,
					reinterpret_cast<wchar_t *>(wctext), maxLengthTextRun - 1);
			} else {	// dbcsMode, so convert using current locale
				wclen = gdk_mbstowcs(
					wctext, s, maxLengthTextRun - 1);
			}
			if (wclen > 0) {
				measure8bit = false;
				wctext[wclen] = L'\0';
				// Map widths back to utf-8 or DBCS input string
				int i = 0;
				for (int iU = 0; iU < wclen; iU++) {
					int width = gdk_char_width_wc(gf, wctext[iU]);
					totalWidth += width;
					size_t lenChar;
					if (et == UTF8) {
						lenChar = UTF8Len(s[i]);
					} else {
						lenChar = mblen(s+i, MB_CUR_MAX);
					}
					while (lenChar--) {
						positions[i++] = totalWidth;
					}
				}
				while (i < len) {	// In case of problems with lengths
					positions[i++] = totalWidth;
				}
			}
		}
		if (measure8bit) {
			// Either Latin1 or conversion failed so treat as Latin1.
			for (int i = 0; i < len; i++) {
				int width = gdk_char_width(gf, s[i]);
				totalWidth += width;
				positions[i] = totalWidth;
			}
		}
	} else {
		// No font so return an ascending range of values
		for (int i = 0; i < len; i++) {
			positions[i] = i + 1;
		}
	}
}

int SurfaceImpl::WidthText(Font &font_, const char *s, int len) {
	if (font_.GetID()) {
#ifdef USE_PANGO
		if (PFont(font_)->pfd) {
			char *utfForm = 0;
			pango_layout_set_font_description(layout, PFont(font_)->pfd);
			PangoRectangle pos;
			if (et == UTF8) {
				pango_layout_set_text(layout, s, len);
			} else {
				if (et == dbcs) {
					// Convert to utf8
					utfForm = UTF8FromDBCS(s, len);
				}
				if (!utfForm)	// Latin1 or DBCS failed so treat as Latin1
					utfForm = UTF8FromLatin1(s, len);
				pango_layout_set_text(layout, utfForm, strlen(utfForm));
			}
			pango_layout_index_to_pos(layout, len, &pos);
			int width = PANGO_PIXELS(pos.x);
			delete []utfForm;
			return width;
		}
#endif
		if (et == UTF8) {
			GdkWChar wctext[maxLengthTextRun];
			size_t wclen = UCS2FromUTF8(s, len, (wchar_t *)wctext, sizeof(wctext) / sizeof(GdkWChar) - 1);
			wctext[wclen] = L'\0';
			return gdk_text_width_wc(PFont(font_)->pfont, wctext, wclen);
		} else {
			return gdk_text_width(PFont(font_)->pfont, s, len);
		}
	} else {
		return 1;
	}
}

int SurfaceImpl::WidthChar(Font &font_, char ch) {
	if (font_.GetID()) {
#ifdef USE_PANGO
		if (PFont(font_)->pfd) {
			return WidthText(font_, &ch, 1);
		}
#endif
		return gdk_char_width(PFont(font_)->pfont, ch);
	} else {
		return 1;
	}
}

// Three possible strategies for determining ascent and descent of font:
// 1) Call gdk_string_extents with string containing all letters, numbers and punctuation.
// 2) Use the ascent and descent fields of GdkFont.
// 3) Call gdk_string_extents with string as 1 but also including accented capitals.
// Smallest values given by 1 and largest by 3 with 2 in between.
// Techniques 1 and 2 sometimes chop off extreme portions of ascenders and
// descenders but are mostly OK except for accented characters like � which are
// rarely used in code.

// This string contains a good range of characters to test for size.
const char largeSizeString[] = "���� `~!@#$%^&*()-_=+\\|[]{};:\"\'<,>.?/1234567890"
                               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
const char sizeString[] = "`~!@#$%^&*()-_=+\\|[]{};:\"\'<,>.?/1234567890"
                          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

int SurfaceImpl::Ascent(Font &font_) {
	if (!(font_.GetID()))
		return 1;
#ifdef FAST_WAY
	FontMutexLock();
	int ascent = PFont(font_)->ascent;
#ifdef USE_PANGO
	if ((ascent == 0) && (PFont(font_)->pfd)) {
		PangoFontMetrics *metrics = pango_context_get_metrics(pcontext,
			PFont(font_)->pfd, pango_context_get_language(pcontext));
		PFont(font_)->ascent =
			PANGO_PIXELS(pango_font_metrics_get_ascent(metrics));
		pango_font_metrics_unref(metrics);
		ascent = PFont(font_)->ascent;
	}
#endif
	if (ascent == 0) {
		ascent = PFont(font_)->pfont->ascent;
	}
	FontMutexUnlock();
	return ascent;
#else

	gint lbearing;
	gint rbearing;
	gint width;
	gint ascent;
	gint descent;

	gdk_string_extents(PFont(font_)->pfont, sizeString,
					   &lbearing, &rbearing, &width, &ascent, &descent);
	return ascent;
#endif
}

int SurfaceImpl::Descent(Font &font_) {
	if (!(font_.GetID()))
		return 1;
#ifdef FAST_WAY

#ifdef USE_PANGO
	if (PFont(font_)->pfd) {
		PangoFontMetrics *metrics = pango_context_get_metrics(pcontext,
			PFont(font_)->pfd, pango_context_get_language(pcontext));
		int descent = PANGO_PIXELS(pango_font_metrics_get_descent(metrics));
		pango_font_metrics_unref(metrics);
		return descent;
	}
#endif
	return PFont(font_)->pfont->descent;
#else

	gint lbearing;
	gint rbearing;
	gint width;
	gint ascent;
	gint descent;

	gdk_string_extents(PFont(font_)->pfont, sizeString,
					   &lbearing, &rbearing, &width, &ascent, &descent);
	return descent;
#endif
}

int SurfaceImpl::InternalLeading(Font &) {
	return 0;
}

int SurfaceImpl::ExternalLeading(Font &) {
	return 0;
}

int SurfaceImpl::Height(Font &font_) {
	return Ascent(font_) + Descent(font_);
}

int SurfaceImpl::AverageCharWidth(Font &font_) {
	return WidthChar(font_, 'n');
}

int SurfaceImpl::SetPalette(Palette *, bool) {
	// Handled in palette allocation for GTK so this does nothing
	return 0;
}

void SurfaceImpl::SetClip(PRectangle rc) {
	GdkRectangle area = {rc.left, rc.top,
	                     rc.right - rc.left, rc.bottom - rc.top};
	gdk_gc_set_clip_rectangle(gc, &area);
}

void SurfaceImpl::FlushCachedState() {}

void SurfaceImpl::SetUnicodeMode(bool unicodeMode_) {
	if (unicodeMode_)
		et = UTF8;
}

void SurfaceImpl::SetDBCSMode(int codePage) {
	if (codePage == SC_CP_DBCS)
		et = dbcs;
}

Surface *Surface::Allocate() {
	return new SurfaceImpl;
}

Window::~Window() {}

void Window::Destroy() {
	if (id)
		gtk_widget_destroy(GTK_WIDGET(id));
	id = 0;
}

bool Window::HasFocus() {
	return GTK_WIDGET_HAS_FOCUS(id);
}

PRectangle Window::GetPosition() {
	// Before any size allocated pretend its 1000 wide so not scrolled
	PRectangle rc(0, 0, 1000, 1000);
	if (id) {
		rc.left = PWidget(id)->allocation.x;
		rc.top = PWidget(id)->allocation.y;
		if (PWidget(id)->allocation.width > 20) {
			rc.right = rc.left + PWidget(id)->allocation.width;
			rc.bottom = rc.top + PWidget(id)->allocation.height;
		}
	}
	return rc;
}

void Window::SetPosition(PRectangle rc) {
#if 1
	//gtk_widget_set_uposition(id, rc.left, rc.top);
	GtkAllocation alloc;
	alloc.x = rc.left;
	alloc.y = rc.top;
	alloc.width = rc.Width();
	alloc.height = rc.Height();
	gtk_widget_size_allocate(PWidget(id), &alloc);
#else

	gtk_widget_set_uposition(id, rc.left, rc.top);
	gtk_widget_set_usize(id, rc.right - rc.left, rc.bottom - rc.top);
#endif
}

void Window::SetPositionRelative(PRectangle rc, Window relativeTo) {
	int ox = 0;
	int oy = 0;
	gdk_window_get_origin(PWidget(relativeTo.id)->window, &ox, &oy);

	gtk_widget_set_uposition(PWidget(id), rc.left + ox, rc.top + oy);
#if 0

	GtkAllocation alloc;
	alloc.x = rc.left + ox;
	alloc.y = rc.top + oy;
	alloc.width = rc.right - rc.left;
	alloc.height = rc.bottom - rc.top;
	gtk_widget_size_allocate(id, &alloc);
#endif

	gtk_widget_set_usize(PWidget(id), rc.right - rc.left, rc.bottom - rc.top);
}

PRectangle Window::GetClientPosition() {
	// On GTK+, the client position is the window position
	return GetPosition();
}

void Window::Show(bool show) {
	if (show)
		gtk_widget_show(PWidget(id));
}

void Window::InvalidateAll() {
	if (id) {
		gtk_widget_queue_draw(PWidget(id));
	}
}

void Window::InvalidateRectangle(PRectangle rc) {
	if (id) {
		gtk_widget_queue_draw_area(PWidget(id),
		                           rc.left, rc.top,
		                           rc.right - rc.left, rc.bottom - rc.top);
	}
}

void Window::SetFont(Font &) {
	// TODO
}

void Window::SetCursor(Cursor curs) {
	// We don't set the cursor to same value numerous times under gtk because
	// it stores the cursor in the window once it's set
	if (curs == cursorLast)
		return;

	cursorLast = curs;
	GdkCursor *gdkCurs;
	switch (curs) {
	case cursorText:
		gdkCurs = gdk_cursor_new(GDK_XTERM);
		break;
	case cursorArrow:
		gdkCurs = gdk_cursor_new(GDK_LEFT_PTR);
		break;
	case cursorUp:
		gdkCurs = gdk_cursor_new(GDK_CENTER_PTR);
		break;
	case cursorWait:
		gdkCurs = gdk_cursor_new(GDK_WATCH);
		break;
	case cursorHand:
		gdkCurs = gdk_cursor_new(GDK_HAND2);
		break;
	case cursorReverseArrow:
		gdkCurs = gdk_cursor_new(GDK_RIGHT_PTR);
		break;
	default:
		gdkCurs = gdk_cursor_new(GDK_LEFT_PTR);
		cursorLast = cursorArrow;
		break;
	}

	gdk_window_set_cursor(PWidget(id)->window, gdkCurs);
	gdk_cursor_destroy(gdkCurs);
}

void Window::SetTitle(const char *s) {
	gtk_window_set_title(GTK_WINDOW(id), s);
}

struct ListImage {
	const char *xpm_data;
	GdkPixmap *pixmap;
	GdkBitmap *bitmap;
};

static void list_image_free(gpointer, gpointer value, gpointer) {
	ListImage *list_image = (ListImage *) value;
	if (list_image->pixmap)
		gdk_pixmap_unref(list_image->pixmap);
	if (list_image->bitmap)
		gdk_bitmap_unref(list_image->bitmap);
	g_free(list_image);
}

ListBox::ListBox() {
}

ListBox::~ListBox() {
}

class ListBoxX : public ListBox {
	WindowID list;
	WindowID scroller;
	int current;
	void *pixhash;
	int lineHeight;
	XPMSet xset;
	bool unicodeMode;
	int desiredVisibleRows;
	unsigned int maxItemCharacters;
	unsigned int aveCharWidth;
public:
	CallBackAction doubleClickAction;
	void *doubleClickActionData;

	ListBoxX() : list(0), current(0), pixhash(NULL), desiredVisibleRows(5), maxItemCharacters(0),
		doubleClickAction(NULL), doubleClickActionData(NULL) {
	}
	virtual ~ListBoxX() {
		if (pixhash) {
			g_hash_table_foreach((GHashTable *) pixhash, list_image_free, NULL);
			g_hash_table_destroy((GHashTable *) pixhash);
		}
	}
	virtual void SetFont(Font &font);
	virtual void Create(Window &parent, int ctrlID, int lineHeight_, bool unicodeMode_);
	virtual void SetAverageCharWidth(int width);
	virtual void SetVisibleRows(int rows);
	virtual PRectangle GetDesiredRect();
	virtual int CaretFromEdge();
	virtual void Clear();
	virtual void Append(char *s, int type = -1);
	virtual int Length();
	virtual void Select(int n);
	virtual int GetSelection();
	virtual int Find(const char *prefix);
	virtual void GetValue(int n, char *value, int len);
	virtual void Sort();
	virtual void RegisterImage(int type, const char *xpm_data);
	virtual void ClearRegisteredImages();
	virtual void SetDoubleClickAction(CallBackAction action, void *data) {
		doubleClickAction = action;
		doubleClickActionData = data;
	}
};

ListBox *ListBox::Allocate() {
	ListBoxX *lb = new ListBoxX();
	return lb;
}

static void SelectionAC(GtkWidget *, gint row, gint,
                        GdkEventButton *, gpointer p) {
	int *pi = reinterpret_cast<int *>(p);
	*pi = row;
}

static gboolean ButtonPress(GtkWidget *, GdkEventButton* ev, gpointer p) {
	ListBoxX* lb = reinterpret_cast<ListBoxX*>(p);
	if (ev->type == GDK_2BUTTON_PRESS && lb->doubleClickAction != NULL) {
		lb->doubleClickAction(lb->doubleClickActionData);
		return TRUE;
	}

	return FALSE;
}

void ListBoxX::Create(Window &, int, int, bool) {
	id = gtk_window_new(GTK_WINDOW_POPUP);

	GtkWidget *frame = gtk_frame_new(NULL);
	gtk_widget_show (frame);
	gtk_container_add(GTK_CONTAINER(GetID()), frame);
	gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_OUT);
	gtk_container_set_border_width(GTK_CONTAINER(frame), 0);

	scroller = gtk_scrolled_window_new(NULL, NULL);
	gtk_container_set_border_width(GTK_CONTAINER(scroller), 0);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
	                               GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(frame), PWidget(scroller));
	gtk_widget_show(PWidget(scroller));

	list = gtk_clist_new(1);
	gtk_widget_show(PWidget(list));
	gtk_container_add(GTK_CONTAINER(PWidget(scroller)), PWidget(list));
	gtk_clist_set_column_auto_resize(GTK_CLIST(PWidget(list)), 0, TRUE);
	gtk_clist_set_selection_mode(GTK_CLIST(PWidget(list)), GTK_SELECTION_BROWSE);
	gtk_signal_connect(GTK_OBJECT(PWidget(list)), "select_row",
	                   GTK_SIGNAL_FUNC(SelectionAC), &current);
	gtk_signal_connect(GTK_OBJECT(PWidget(list)), "button_press_event",
	                   GTK_SIGNAL_FUNC(ButtonPress), this);
	gtk_clist_set_shadow_type(GTK_CLIST(PWidget(list)), GTK_SHADOW_NONE);

	gtk_widget_realize(PWidget(id));
}

void ListBoxX::SetFont(Font &scint_font) {
#if GTK_MAJOR_VERSION < 2
	GtkStyle *style = gtk_widget_get_style(GTK_WIDGET(PWidget(list)));
	if (!gdk_font_equal(style->font, PFont(scint_font)->pfont)) {
		style = gtk_style_copy(style);
		gdk_font_unref(style->font);
		style->font = PFont(scint_font)->pfont;
		gdk_font_ref(style->font);
		gtk_widget_set_style(GTK_WIDGET(PWidget(list)), style);
		gtk_style_unref(style);
	}
#else
	GtkStyle *styleCurrent = gtk_widget_get_style(GTK_WIDGET(PWidget(list)));
	GdkFont *fontCurrent = gtk_style_get_font(styleCurrent);
	if (!gdk_font_equal(fontCurrent, PFont(scint_font)->pfont)) {
		GtkStyle *styleNew = gtk_style_copy(styleCurrent);
		gtk_style_set_font(styleNew, PFont(scint_font)->pfont);
		gtk_widget_set_style(GTK_WIDGET(PWidget(list)), styleNew);
		gtk_style_unref(styleCurrent);
	}
#endif
}

void ListBoxX::SetAverageCharWidth(int width) {
	aveCharWidth = width;
}

void ListBoxX::SetVisibleRows(int rows) {
	desiredVisibleRows = rows;
}

PRectangle ListBoxX::GetDesiredRect() {
	// Before any size allocated pretend its 100 wide so not scrolled
	PRectangle rc(0, 0, 100, 100);
	if (id) {
		int rows = Length();
		if ((rows == 0) || (rows > desiredVisibleRows))
			rows = desiredVisibleRows;

		GtkRequisition req;
		int height;

#if GTK_MAJOR_VERSION < 2

		int ythickness = PWidget(list)->style->klass->ythickness;
#else

		int ythickness = PWidget(list)->style->ythickness;
#endif
		// First calculate height of the clist for our desired visible row count otherwise it tries to expand to the total # of rows
		height = (rows * GTK_CLIST(list)->row_height
		          + rows + 1
		          + 2 * (ythickness
		                 + GTK_CONTAINER(PWidget(list))->border_width));
		gtk_widget_set_usize(GTK_WIDGET(PWidget(list)), -1, height);

		// Get the size of the scroller because we set usize on the window
		gtk_widget_size_request(GTK_WIDGET(scroller), &req);
		rc.right = req.width;
		rc.bottom = req.height;

		gtk_widget_set_usize(GTK_WIDGET(list), -1, -1);
		int width = maxItemCharacters;
		if (width < 12)
			width = 12;
		rc.right = width * (aveCharWidth + aveCharWidth / 3);
		if (Length() > rows)
			rc.right = rc.right + 16;
	}
	return rc;
}

int ListBoxX::CaretFromEdge() {
	return 4 + xset.GetWidth();
}

void ListBoxX::Clear() {
	gtk_clist_clear(GTK_CLIST(list));
	maxItemCharacters = 0;
}

static void init_pixmap(ListImage *list_image, GtkWidget *window) {
	const char *textForm = list_image->xpm_data;
	const char * const * xpm_lineform = reinterpret_cast<const char * const *>(textForm);
	const char **xpm_lineformfromtext = 0;
	// The XPM data can be either in atext form as will be read from a file
	// or in a line form (array of char  *) as will be used for images defined in code.
	// Test for text form and convert to line form
	if ((0 == memcmp(textForm, "/* X", 4)) && (0 == memcmp(textForm, "/* XPM */", 9))) {
		// Test done is two parts to avoid possibility of overstepping the memory
		// if memcmp implemented strangely. Must be 4 bytes at least at destination.
		xpm_lineformfromtext = XPM::LinesFormFromTextForm(textForm);
		xpm_lineform = xpm_lineformfromtext;
	}

	// Drop any existing pixmap/bitmap as data may have changed
	if (list_image->pixmap)
		gdk_pixmap_unref(list_image->pixmap);
	list_image->pixmap = NULL;
	if (list_image->bitmap)
		gdk_bitmap_unref(list_image->bitmap);
	list_image->bitmap = NULL;

	list_image->pixmap = gdk_pixmap_colormap_create_from_xpm_d(NULL
	             , gtk_widget_get_colormap(window), &(list_image->bitmap), NULL
	             , (gchar **) xpm_lineform);
	if (NULL == list_image->pixmap) {
		if (list_image->bitmap)
			gdk_bitmap_unref(list_image->bitmap);
		list_image->bitmap = NULL;
	}
	delete []xpm_lineformfromtext;
}

#define SPACING 5

void ListBoxX::Append(char *s, int type) {
	char * szs[] = { s, NULL };
	ListImage *list_image = NULL;
	if ((type >= 0) && pixhash) {
		list_image = (ListImage *) g_hash_table_lookup((GHashTable *) pixhash
		             , (gconstpointer) GINT_TO_POINTER(type));
	}
	int rownum = gtk_clist_append(GTK_CLIST(list), szs);
	if (list_image) {
		if (NULL == list_image->pixmap)
			init_pixmap(list_image, (GtkWidget *) list);
		gtk_clist_set_pixtext(GTK_CLIST(list), rownum, 0, s, SPACING
		                      , list_image->pixmap, list_image->bitmap);
	}
	size_t len = strlen(s);
	if (maxItemCharacters < len)
		maxItemCharacters = len;
}

int ListBoxX::Length() {
	if (id)
		return GTK_CLIST(list)->rows;
	return 0;
}

void ListBoxX::Select(int n) {
	gtk_clist_select_row(GTK_CLIST(list), n, 0);
	gtk_clist_moveto(GTK_CLIST(list), n, 0, 0.5, 0.5);
}

int ListBoxX::GetSelection() {
	return current;
}

int ListBoxX::Find(const char *prefix) {
	int count = Length();
	for (int i = 0; i < count; i++) {
		char *s = 0;
		gtk_clist_get_text(GTK_CLIST(list), i, 0, &s);
		if (s && (0 == strncmp(prefix, s, strlen(prefix)))) {
			return i;
		}
	}
	return - 1;
}

void ListBoxX::GetValue(int n, char *value, int len) {
	char *text = NULL;
	GtkCellType type = gtk_clist_get_cell_type(GTK_CLIST(list), n, 0);
	switch (type) {
	case GTK_CELL_TEXT:
		gtk_clist_get_text(GTK_CLIST(list), n, 0, &text);
		break;
	case GTK_CELL_PIXTEXT:
		gtk_clist_get_pixtext(GTK_CLIST(list), n, 0, &text, NULL, NULL, NULL);
		break;
	default:
		break;
	}
	if (text && len > 0) {
		strncpy(value, text, len);
		value[len - 1] = '\0';
	} else {
		value[0] = '\0';
	}
}

void ListBoxX::Sort() {
	gtk_clist_sort(GTK_CLIST(list));
}

// g_return_if_fail causes unnecessary compiler warning in release compile.
#ifdef _MSC_VER
#pragma warning(disable: 4127)
#endif

void ListBoxX::RegisterImage(int type, const char *xpm_data) {
	g_return_if_fail(xpm_data);

	// Saved and use the saved copy so caller's copy can disappear.
	xset.Add(type, xpm_data);
	XPM *pxpm = xset.Get(type);
	xpm_data = reinterpret_cast<const char *>(pxpm->InLinesForm());

	if (!pixhash) {
		pixhash = g_hash_table_new(g_direct_hash, g_direct_equal);
	}
	ListImage *list_image = (ListImage *) g_hash_table_lookup((GHashTable *) pixhash,
		(gconstpointer) GINT_TO_POINTER(type));
	if (list_image) {
		// Drop icon already registered
		if (list_image->pixmap)
			gdk_pixmap_unref(list_image->pixmap);
		list_image->pixmap = 0;
		if (list_image->bitmap)
			gdk_bitmap_unref(list_image->bitmap);
		list_image->bitmap = 0;
		list_image->xpm_data = xpm_data;
	} else {
		list_image = g_new0(ListImage, 1);
		list_image->xpm_data = xpm_data;
		g_hash_table_insert((GHashTable *) pixhash, GINT_TO_POINTER(type),
			(gpointer) list_image);
	}
}

void ListBoxX::ClearRegisteredImages() {
	xset.Clear();
}

Menu::Menu() : id(0) {}

void Menu::CreatePopUp() {
	Destroy();
	id = gtk_item_factory_new(GTK_TYPE_MENU, "<main>", NULL);
}

void Menu::Destroy() {
	if (id)
		gtk_object_unref(GTK_OBJECT(id));
	id = 0;
}

void Menu::Show(Point pt, Window &) {
	int screenHeight = gdk_screen_height();
	int screenWidth = gdk_screen_width();
	GtkItemFactory *factory = reinterpret_cast<GtkItemFactory *>(id);
	GtkWidget *widget = gtk_item_factory_get_widget(factory, "<main>");
	gtk_widget_show_all(widget);
	GtkRequisition requisition;
	gtk_widget_size_request(widget, &requisition);
	if ((pt.x + requisition.width) > screenWidth) {
		pt.x = screenWidth - requisition.width;
	}
	if ((pt.y + requisition.height) > screenHeight) {
		pt.y = screenHeight - requisition.height;
	}
	gtk_item_factory_popup(factory, pt.x - 4, pt.y, 3, 0);
}

ElapsedTime::ElapsedTime() {
	GTimeVal curTime;
	g_get_current_time(&curTime);
	bigBit = curTime.tv_sec;
	littleBit = curTime.tv_usec;
}

double ElapsedTime::Duration(bool reset) {
	GTimeVal curTime;
	g_get_current_time(&curTime);
	long endBigBit = curTime.tv_sec;
	long endLittleBit = curTime.tv_usec;
	double result = 1000000.0 * (endBigBit - bigBit);
	result += endLittleBit - littleBit;
	result /= 1000000.0;
	if (reset) {
		bigBit = endBigBit;
		littleBit = endLittleBit;
	}
	return result;
}

ColourDesired Platform::Chrome() {
	return ColourDesired(0xe0, 0xe0, 0xe0);
}

ColourDesired Platform::ChromeHighlight() {
	return ColourDesired(0xff, 0xff, 0xff);
}

const char *Platform::DefaultFont() {
#ifdef G_OS_WIN32
	return "Lucida Console";
#else

	return "lucidatypewriter";
#endif
}

int Platform::DefaultFontSize() {
#ifdef G_OS_WIN32
	return 10;
#else

	return 12;
#endif
}

unsigned int Platform::DoubleClickTime() {
	return 500; 	// Half a second
}

bool Platform::MouseButtonBounce() {
	return true;
}

void Platform::DebugDisplay(const char *s) {
	printf("%s", s);
}

bool Platform::IsKeyDown(int) {
	// TODO: discover state of keys in GTK+/X
	return false;
}

long Platform::SendScintilla(
    WindowID w, unsigned int msg, unsigned long wParam, long lParam) {
	return scintilla_send_message(SCINTILLA(w), msg, wParam, lParam);
}

long Platform::SendScintillaPointer(
    WindowID w, unsigned int msg, unsigned long wParam, void *lParam) {
	return scintilla_send_message(SCINTILLA(w), msg, wParam,
	                              reinterpret_cast<sptr_t>(lParam));
}

bool Platform::IsDBCSLeadByte(int /*codePage*/, char /*ch*/) {
	return false;
}

int Platform::DBCSCharLength(int /*codePage*/, const char *s) {
	int bytes = mblen(s, MB_CUR_MAX);
	if (bytes >= 1)
		return bytes;
	else
		return 1;
}

int Platform::DBCSCharMaxLength() {
	return MB_CUR_MAX;
}

// These are utility functions not really tied to a platform

int Platform::Minimum(int a, int b) {
	if (a < b)
		return a;
	else
		return b;
}

int Platform::Maximum(int a, int b) {
	if (a > b)
		return a;
	else
		return b;
}

//#define TRACE

#ifdef TRACE
void Platform::DebugPrintf(const char *format, ...) {
	char buffer[2000];
	va_list pArguments;
	va_start(pArguments, format);
	vsprintf(buffer, format, pArguments);
	va_end(pArguments);
	Platform::DebugDisplay(buffer);
}
#else
void Platform::DebugPrintf(const char *, ...) {}

#endif

// Not supported for GTK+
static bool assertionPopUps = true;

bool Platform::ShowAssertionPopUps(bool assertionPopUps_) {
	bool ret = assertionPopUps;
	assertionPopUps = assertionPopUps_;
	return ret;
}

void Platform::Assert(const char *c, const char *file, int line) {
	char buffer[2000];
	sprintf(buffer, "Assertion [%s] failed at %s %d", c, file, line);
	strcat(buffer, "\r\n");
	Platform::DebugDisplay(buffer);
	abort();
}

int Platform::Clamp(int val, int minVal, int maxVal) {
	if (val > maxVal)
		val = maxVal;
	if (val < minVal)
		val = minVal;
	return val;
}

void Platform_Initialise() {
	FontMutexAllocate();
}

void Platform_Finalise() {
	FontMutexFree();
}
