// ============================================================
//  PDF Reader  --  gtkmm-4.0 + poppler-glib
//
//  Features:
//    - Multi-tab (open many PDFs at once)
//    - Drag-and-drop PDF files onto window to open
//    - Full keyboard shortcuts (see SHORTCUTS below)
//    - Background threaded rendering (no UI freeze)
//    - Debounced zoom/rotate
//    - Viewport culling + prefetch
//
//  SHORTCUTS:
//    Ctrl+O          Open PDF (new tab)
//    Ctrl+W          Close current tab
//    Ctrl+Tab        Next tab
//    Ctrl+Shift+Tab  Previous tab
//    Ctrl+F          Find text
//    Ctrl++          Zoom in
//    Ctrl+-          Zoom out
//    Ctrl+0          Zoom 100%
//    Ctrl+Shift+W    Fit to width
//    R               Rotate clockwise
//    Shift+R         Rotate counter-clockwise
//    Up/Down         Scroll smoothly
//    Page Up/Down    Scroll one viewport
//    Home/End        Jump to start/end
//    F11             Full screen
//    B               Toggle bookmarks sidebar
//    Escape          Close search / exit fullscreen
//
//  Build (MSYS2 UCRT64):
//    g++ pdf_reader.cpp -o pdf_reader
//        $(pkg-config --cflags --libs gtkmm-4.0 poppler-glib)
//        -std=c++17 -O2 -lpthread
// ============================================================

#define _USE_MATH_DEFINES
#include <cmath>
#include <gtkmm.h>
#include <gdkmm.h>
#include <poppler/glib/poppler.h>
#include <cairo.h>

#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <memory>

static std::string itos(int v) { return std::to_string(v); }

// ============================================================
//  CSS
// ============================================================
static const char* APP_CSS = R"CSS(

/* ── Toolbar ─────────────────────────────────────────────── */
.pdf-toolbar {
    background-color: @headerbar_bg_color;
    border-bottom: 1px solid alpha(@borders, 0.8);
    padding: 3px 8px;
    min-height: 40px;
}
.pdf-toolbar > button,
.pdf-toolbar button {
    min-width:  32px;
    min-height: 32px;
    padding: 0;
    border-radius: 6px;
    background: none;
    border: none;
    box-shadow: none;
    color: #000000;
    font-size: 1.05em;
}
.pdf-toolbar > button:hover,
.pdf-toolbar button:hover   { background-color: alpha(@accent_bg_color, 0.15); }
.pdf-toolbar > button:active,
.pdf-toolbar button:active  { background-color: alpha(@accent_bg_color, 0.28); }
.pdf-toolbar > button:disabled,
.pdf-toolbar button:disabled { opacity: 0.35; }
.pdf-toolbar .sep {
    min-width: 1px; min-height: 20px;
    background-color: alpha(@borders, 0.6);
    margin: 0 5px;
}
.page-spin  { min-width: 46px; max-width: 52px; min-height: 28px; }
.page-label { font-size: 0.88em; color: #000000; padding: 0 2px; }
.zoom-label { font-size: 0.85em; color: #000000; min-width: 44px; padding: 0 2px; }

/* ── Notebook / tabs ─────────────────────────────────────── */
notebook > header {
    background-color: @headerbar_bg_color;
    border-bottom: 1px solid alpha(@borders, 0.8);
    padding: 2px 4px 0 4px;
}
notebook > header > tabs > tab {
    padding: 4px 10px;
    min-width: 80px;
    max-width: 200px;
    font-size: 0.88em;
    color: #000000;
    border-radius: 6px 6px 0 0;
}
notebook > header > tabs > tab:checked {
    background-color: @window_bg_color;
    font-weight: bold;
}
/* close button inside tab */
.tab-close {
    padding: 0 2px;
    min-width: 18px;
    min-height: 18px;
    border-radius: 4px;
    background: none;
    border: none;
    box-shadow: none;
    font-size: 0.78em;
    color: alpha(#000000, 0.55);
}
.tab-close:hover  { background-color: alpha(#000000, 0.12); color: #000000; }
.tab-close:active { background-color: alpha(#000000, 0.22); }

/* ── Search bar ──────────────────────────────────────────── */
.search-bar-box {
    background-color: @headerbar_bg_color;
    border-bottom: 1px solid alpha(@borders, 0.8);
    padding: 4px 10px;
}
.search-bar-box entry { min-width: 200px; }
.search-count { font-size: 0.82em; color: #000000; min-width: 60px; padding: 0 6px; }

/* ── Sidebar ─────────────────────────────────────────────── */
.pdf-sidebar { background-color: @sidebar_bg_color; border-right: 1px solid alpha(@borders,0.7); }
.sidebar-title {
    font-weight: bold; font-size: 0.76em; letter-spacing: 0.07em;
    color: #000000; padding: 10px 12px 5px 12px;
}

/* ── Status bar ──────────────────────────────────────────── */
.pdf-statusbar {
    background-color: @headerbar_bg_color;
    border-top: 1px solid alpha(@borders, 0.6);
    padding: 0 12px; min-height: 22px;
    font-size: 0.76em; color: #000000;
}

/* ── Drop highlight ──────────────────────────────────────── */
.drop-highlight {
    border: 3px dashed @accent_bg_color;
}

)CSS";

// ============================================================
//  PageSize cache
// ============================================================
struct PageSize { double w = 0, h = 0; };

// ============================================================
//  RenderedPage
// ============================================================
struct RenderedPage {
    int                        index   = -1;
    double                     at_zoom = 0;
    int                        at_rot  = -1;
    cairo_surface_t*           surf    = nullptr;
    Glib::RefPtr<Gdk::Pixbuf>  pixbuf;
    int                        w = 0, h = 0;

    RenderedPage() = default;
    RenderedPage(const RenderedPage&)            = delete;
    RenderedPage& operator=(const RenderedPage&) = delete;
    RenderedPage(RenderedPage&& o) noexcept
        : index(o.index), at_zoom(o.at_zoom), at_rot(o.at_rot),
          surf(o.surf), pixbuf(std::move(o.pixbuf)), w(o.w), h(o.h)
    { o.surf = nullptr; }
    RenderedPage& operator=(RenderedPage&& o) noexcept {
        if (this != &o) {
            reset();
            index = o.index; at_zoom = o.at_zoom; at_rot = o.at_rot;
            surf = o.surf; o.surf = nullptr;
            pixbuf = std::move(o.pixbuf); w = o.w; h = o.h;
        }
        return *this;
    }
    ~RenderedPage() { reset(); }

    void reset() {
        pixbuf.reset();
        if (surf) { cairo_surface_destroy(surf); surf = nullptr; }
        w = h = 0; at_zoom = 0; at_rot = -1;
    }
    bool valid_for(double zoom, int rot) const {
        return surf && std::abs(at_zoom - zoom) < 1e-9 && at_rot == rot;
    }
    bool render(PopplerPage* page, double scale, int rotation, const PageSize& ps) {
        reset();
        bool rotated = (rotation == 90 || rotation == 270);
        double rw = rotated ? ps.h : ps.w, rh = rotated ? ps.w : ps.h;
        w = std::max(1, (int)(rw * scale));
        h = std::max(1, (int)(rh * scale));
        surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(surf); surf = nullptr; return false;
        }
        cairo_t* cr = cairo_create(surf);
        cairo_set_source_rgb(cr, 1, 1, 1); cairo_paint(cr);
        cairo_scale(cr, scale, scale);
        switch (rotation) {
            case  90: cairo_translate(cr, ps.h, 0);    cairo_rotate(cr,  M_PI/2); break;
            case 180: cairo_translate(cr, ps.w, ps.h); cairo_rotate(cr,  M_PI);   break;
            case 270: cairo_translate(cr, 0,    ps.w); cairo_rotate(cr, -M_PI/2); break;
        }
        poppler_page_render(page, cr);
        cairo_destroy(cr);
        at_zoom = scale; at_rot = rotation;
        return true;
    }
    void build_pixbuf() {
        if (!surf) return;
        pixbuf = Gdk::Pixbuf::create_from_data(
            cairo_image_surface_get_data(surf),
            Gdk::Colorspace::RGB, true, 8,
            w, h, cairo_image_surface_get_stride(surf));
    }
};

// ============================================================
//  SearchHit / RenderJob
// ============================================================
struct SearchHit  { int page_idx; PopplerRectangle rect; };
struct RenderJob  { int page_idx=-1; double zoom=1; int rotation=0; uint64_t gen=0; };

static const int    PAGE_GAP    = 14;
static const double SCROLL_STEP = 80.0;

// ============================================================
//  MultiPageView  (unchanged from previous version)
// ============================================================
class MultiPageView : public Gtk::DrawingArea {
public:
    MultiPageView() {
        set_expand(true);
        set_draw_func(sigc::mem_fun(*this, &MultiPageView::on_draw));
    }
    ~MultiPageView() override { stop_worker(); free_doc(); }

    bool load_document(const std::string& path) {
        cancel_pending(); free_doc();
        GError* err = nullptr;
        gchar*  uri = g_filename_to_uri(path.c_str(), nullptr, &err);
        if (!uri) { if (err) g_error_free(err); return false; }
        m_doc = poppler_document_new_from_file(uri, nullptr, &err);
        g_free(uri);
        if (!m_doc) { if (err) g_error_free(err); return false; }
        int n = poppler_document_get_n_pages(m_doc);
        m_page_sizes.resize(n); m_pages.resize(n);
        for (int i = 0; i < n; ++i) {
            PopplerPage* pg = poppler_document_get_page(m_doc, i);
            if (pg) { poppler_page_get_size(pg, &m_page_sizes[i].w, &m_page_sizes[i].h); g_object_unref(pg); }
        }
        m_zoom = 1.0; m_rotation = 0;
        recompute_layout(); enqueue_visible_pages();
        return true;
    }

    bool   has_doc()      const { return m_doc != nullptr; }
    int    total_pages()  const { return m_doc ? (int)m_page_sizes.size() : 0; }
    int    current_page() const { return m_visible_page; }
    double zoom()         const { return m_zoom; }
    PopplerDocument* doc()const { return m_doc; }

    void zoom_in()    { request_zoom(m_zoom * 1.20); }
    void zoom_out()   { request_zoom(m_zoom / 1.20); }
    void zoom_100()   { request_zoom(1.0); }
    void zoom_fit_w() {
        if (!m_doc || m_page_sizes.empty()) return;
        bool rotated = (m_rotation==90||m_rotation==270);
        double ref_w = rotated ? m_page_sizes[0].h : m_page_sizes[0].w;
        int aw = get_width(); if (aw < 10) aw = 800;
        request_zoom((aw - 40.0) / ref_w);
    }
    void rotate_cw()  { request_rotate((m_rotation + 90)  % 360); }
    void rotate_ccw() { request_rotate((m_rotation + 270) % 360); }

    void scroll_to_page(int idx) {
        if (!m_doc || idx < 0 || idx >= total_pages()) return;
        m_sig_request_scroll.emit(idx);
    }
    void notify_visible_page(int idx) {
        if (idx == m_visible_page) return;
        m_visible_page = idx;
        m_sig_page_changed.emit(m_visible_page, total_pages());
        enqueue_visible_pages();
    }
    void notify_scroll_position(double top, double view_h) {
        m_viewport_top = top; m_viewport_h = view_h;
        enqueue_visible_pages();
    }

    int search_all(const std::string& text) {
        m_hits.clear(); m_hit_idx = -1; m_search_text = text;
        if (!m_doc || text.empty()) { queue_draw(); return 0; }
        int n = total_pages();
        for (int i = 0; i < n; ++i) {
            PopplerPage* pg = poppler_document_get_page(m_doc, i);
            if (!pg) continue;
            GList* list = poppler_page_find_text(pg, text.c_str());
            for (GList* l = list; l; l = l->next) {
                SearchHit h; h.page_idx = i;
                h.rect = *reinterpret_cast<PopplerRectangle*>(l->data);
                m_hits.push_back(h);
            }
            g_list_free_full(list, reinterpret_cast<GDestroyNotify>(poppler_rectangle_free));
            g_object_unref(pg);
        }
        if (!m_hits.empty()) { m_hit_idx = 0; scroll_to_page(m_hits[0].page_idx); }
        queue_draw(); return (int)m_hits.size();
    }
    void search_next() {
        if (m_hits.empty()) return;
        m_hit_idx = (m_hit_idx+1) % (int)m_hits.size();
        scroll_to_page(m_hits[m_hit_idx].page_idx); queue_draw();
        m_sig_search_changed.emit(m_hit_idx, (int)m_hits.size());
    }
    void search_prev() {
        if (m_hits.empty()) return;
        m_hit_idx = (m_hit_idx-1+(int)m_hits.size()) % (int)m_hits.size();
        scroll_to_page(m_hits[m_hit_idx].page_idx); queue_draw();
        m_sig_search_changed.emit(m_hit_idx, (int)m_hits.size());
    }
    void clear_search() { m_hits.clear(); m_hit_idx=-1; m_search_text.clear(); queue_draw(); }
    int  hit_count() const { return (int)m_hits.size(); }
    int  hit_index() const { return m_hit_idx; }

    sigc::signal<void(int,int)> signal_page_changed()   { return m_sig_page_changed; }
    sigc::signal<void(int,int)> signal_search_changed() { return m_sig_search_changed; }
    sigc::signal<void(int)>     signal_request_scroll() { return m_sig_request_scroll; }

    int page_y_offset(int idx) const {
        if (idx <= 0 || idx >= (int)m_page_y.size()) return PAGE_GAP;
        return m_page_y[idx];
    }

private:
    PopplerDocument*          m_doc          = nullptr;
    std::vector<PageSize>     m_page_sizes;
    std::vector<int>          m_page_y;
    int                       m_total_h = 0, m_max_w = 0;
    double                    m_zoom = 1.0;
    int                       m_rotation = 0, m_visible_page = 0;
    double                    m_viewport_top = 0, m_viewport_h = 600;

    std::vector<RenderedPage> m_pages;
    std::mutex                m_pages_mutex;

    sigc::connection          m_debounce_conn;
    double                    m_pending_zoom = 1.0;
    int                       m_pending_rot  = 0;

    std::string               m_search_text;
    std::vector<SearchHit>    m_hits;
    int                       m_hit_idx = -1;

    sigc::signal<void(int,int)> m_sig_page_changed;
    sigc::signal<void(int,int)> m_sig_search_changed;
    sigc::signal<void(int)>     m_sig_request_scroll;

    std::thread               m_worker;
    std::mutex                m_queue_mutex;
    std::condition_variable   m_queue_cv;
    std::vector<RenderJob>    m_queue;
    std::atomic<bool>         m_worker_stop{false};
    std::atomic<uint64_t>     m_gen{0};

    void start_worker() {
        if (m_worker.joinable()) return;
        m_worker_stop = false;
        m_worker = std::thread([this]{ worker_loop(); });
    }
    void stop_worker() {
        m_worker_stop = true; m_queue_cv.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }
    void cancel_pending() {
        ++m_gen;
        std::lock_guard<std::mutex> lk(m_queue_mutex);
        m_queue.clear();
    }
    void push_job(int idx, bool hi) {
        RenderJob job{idx, m_zoom, m_rotation, m_gen.load()};
        std::lock_guard<std::mutex> lk(m_queue_mutex);
        for (auto& j : m_queue) if (j.page_idx==idx && j.gen==job.gen) return;
        if (hi) m_queue.insert(m_queue.begin(), job);
        else    m_queue.push_back(job);
        m_queue_cv.notify_one();
    }
    void worker_loop() {
        while (!m_worker_stop) {
            RenderJob job;
            {
                std::unique_lock<std::mutex> lk(m_queue_mutex);
                m_queue_cv.wait(lk, [this]{ return m_worker_stop || !m_queue.empty(); });
                if (m_worker_stop) break;
                job = m_queue.front(); m_queue.erase(m_queue.begin());
            }
            if (job.gen != m_gen.load() || !m_doc) continue;
            if (job.page_idx < 0 || job.page_idx >= (int)m_page_sizes.size()) continue;
            {
                std::lock_guard<std::mutex> lk(m_pages_mutex);
                if (m_pages[job.page_idx].valid_for(job.zoom, job.rotation)) continue;
            }
            PopplerPage* pg = poppler_document_get_page(m_doc, job.page_idx);
            if (!pg) continue;
            auto* rp = new RenderedPage();
            rp->index = job.page_idx;
            bool ok = rp->render(pg, job.zoom, job.rotation, m_page_sizes[job.page_idx]);
            g_object_unref(pg);
            if (!ok || job.gen != m_gen.load()) { delete rp; continue; }
            int idx = job.page_idx; uint64_t gen = job.gen;
            Glib::signal_idle().connect_once([this, rp, idx, gen]() {
                std::unique_ptr<RenderedPage> owned(rp);
                if (gen != m_gen.load()) return;
                owned->build_pixbuf();
                std::lock_guard<std::mutex> lk(m_pages_mutex);
                m_pages[idx] = std::move(*owned);
                queue_draw();
            });
        }
    }

    void recompute_layout() {
        int n = (int)m_page_sizes.size();
        m_page_y.resize(n); m_total_h = PAGE_GAP; m_max_w = 0;
        bool rotated = (m_rotation==90||m_rotation==270);
        for (int i = 0; i < n; ++i) {
            m_page_y[i] = m_total_h;
            double pw = rotated ? m_page_sizes[i].h : m_page_sizes[i].w;
            double ph = rotated ? m_page_sizes[i].w : m_page_sizes[i].h;
            int pw_px = std::max(1,(int)(pw*m_zoom)), ph_px = std::max(1,(int)(ph*m_zoom));
            m_total_h += ph_px + PAGE_GAP;
            m_max_w = std::max(m_max_w, pw_px);
        }
        set_size_request(m_max_w+40, m_total_h);
        m_sig_page_changed.emit(m_visible_page, n);
    }
    void enqueue_visible_pages() {
        if (!m_doc) return;
        start_worker();
        int n = (int)m_page_sizes.size();
        bool rotated = (m_rotation==90||m_rotation==270);
        double top = m_viewport_top - m_viewport_h;
        double bot = m_viewport_top + m_viewport_h * 2.0;
        for (int i = 0; i < n; ++i) {
            int pt = m_page_y[i];
            double ph = rotated ? m_page_sizes[i].w : m_page_sizes[i].h;
            int pb = pt + (int)(ph * m_zoom);
            if (pb < top || pt > bot) continue;
            { std::lock_guard<std::mutex> lk(m_pages_mutex);
              if (m_pages[i].valid_for(m_zoom, m_rotation)) continue; }
            bool vis = (pb >= m_viewport_top && pt <= m_viewport_top + m_viewport_h);
            push_job(i, vis);
        }
    }
    void request_zoom(double z) {
        m_pending_zoom = std::max(0.08, std::min(z, 8.0)); m_pending_rot = m_rotation;
        arm_debounce();
    }
    void request_rotate(int rot) {
        m_pending_rot = rot; m_pending_zoom = m_zoom; arm_debounce();
    }
    void arm_debounce() {
        m_debounce_conn.disconnect();
        m_zoom = m_pending_zoom; m_rotation = m_pending_rot;
        recompute_layout(); queue_draw();
        m_debounce_conn = Glib::signal_timeout().connect([this]() -> bool {
            cancel_pending();
            { std::lock_guard<std::mutex> lk(m_pages_mutex);
              for (auto& p : m_pages) p.reset(); }
            enqueue_visible_pages(); return false;
        }, 80);
    }
    void free_doc() {
        m_debounce_conn.disconnect();
        { std::lock_guard<std::mutex> lk(m_pages_mutex); m_pages.clear(); }
        m_page_sizes.clear(); m_page_y.clear();
        m_hits.clear(); m_hit_idx = -1; m_visible_page = 0;
        m_total_h = 0; m_max_w = 0;
        if (m_doc) { g_object_unref(m_doc); m_doc = nullptr; }
    }

    void on_draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int) {
        cr->set_source_rgb(0.17, 0.17, 0.17); cr->paint();
        std::lock_guard<std::mutex> lk(m_pages_mutex);
        int n = (int)m_pages.size(); if (n==0) return;
        bool rotated = (m_rotation==90||m_rotation==270);
        for (int i = 0; i < n; ++i) {
            int y = m_page_y[i];
            double pw_pt = rotated?m_page_sizes[i].h:m_page_sizes[i].w;
            double ph_pt = rotated?m_page_sizes[i].w:m_page_sizes[i].h;
            int pw_px = std::max(1,(int)(pw_pt*m_zoom));
            int ph_px = std::max(1,(int)(ph_pt*m_zoom));
            int ox = std::max(0,(w-pw_px)/2);
            cr->set_source_rgba(0,0,0,0.28);
            cr->rectangle(ox+3, y+3, pw_px, ph_px); cr->fill();
            if (m_pages[i].pixbuf) {
                Gdk::Cairo::set_source_pixbuf(cr, m_pages[i].pixbuf, ox, y);
                cr->paint();
            } else {
                cr->set_source_rgb(0.96,0.96,0.96);
                cr->rectangle(ox, y, pw_px, ph_px); cr->fill();
                cr->set_source_rgba(0.5,0.5,0.5,0.7);
                cr->select_font_face("Sans", Cairo::ToyFontFace::Slant::NORMAL,
                                             Cairo::ToyFontFace::Weight::NORMAL);
                cr->set_font_size(13);
                Cairo::TextExtents te; cr->get_text_extents("Rendering…", te);
                cr->move_to(ox+(pw_px-te.width)/2.0, y+(ph_px+te.height)/2.0);
                cr->show_text("Rendering…");
            }
            draw_search_highlights(cr, i, ox, y);
        }
    }
    void draw_search_highlights(const Cairo::RefPtr<Cairo::Context>& cr,
                                int pi, int ox, int oy) {
        if (m_hits.empty()) return;
        double ph_pt = m_page_sizes[pi].h;
        for (int j = 0; j < (int)m_hits.size(); ++j) {
            const auto& h = m_hits[j]; if (h.page_idx != pi) continue;
            double s=m_zoom, x1=h.rect.x1*s+ox, y1=(ph_pt-h.rect.y2)*s+oy;
            double bw=(h.rect.x2-h.rect.x1)*s, bh=(h.rect.y2-h.rect.y1)*s;
            cr->set_source_rgba(0.98,0.82,0.10,(j==m_hit_idx)?0.82:0.35);
            cr->rectangle(x1,y1,bw,bh); cr->fill();
        }
    }
};

// ============================================================
//  SearchBar
// ============================================================
class SearchBar : public Gtk::Box {
public:
    sigc::signal<void(const std::string&)> signal_search;
    sigc::signal<void()> signal_next, signal_prev, signal_close;

    SearchBar() : Gtk::Box(Gtk::Orientation::HORIZONTAL, 4) {
        add_css_class("search-bar-box");
        set_margin_start(4); set_margin_end(4);
        auto* icon = Gtk::make_managed<Gtk::Label>("🔍");
        icon->set_margin_end(2); append(*icon);
        m_entry.set_placeholder_text("Search in document…");
        m_entry.set_hexpand(true);
        m_entry.signal_changed().connect([this]{ signal_search.emit(m_entry.get_text()); });
        m_entry.signal_activate().connect([this]{ signal_next.emit(); });
        auto key = Gtk::EventControllerKey::create();
        key->signal_key_pressed().connect([this](guint k, guint, Gdk::ModifierType)->bool{
            if (k==GDK_KEY_Escape) { signal_close.emit(); return true; }
            if (k==GDK_KEY_Return||k==GDK_KEY_KP_Enter) { signal_next.emit(); return true; }
            return false;
        }, false);
        m_entry.add_controller(key);
        m_lbl_count.add_css_class("search-count");
        m_btn_prev.set_label("▲"); m_btn_prev.set_tooltip_text("Previous match");
        m_btn_prev.signal_clicked().connect([this]{ signal_prev.emit(); });
        m_btn_next.set_label("▼"); m_btn_next.set_tooltip_text("Next match");
        m_btn_next.signal_clicked().connect([this]{ signal_next.emit(); });
        m_btn_close.set_label("✕"); m_btn_close.set_tooltip_text("Close  Esc");
        m_btn_close.signal_clicked().connect([this]{ signal_close.emit(); });
        append(m_entry); append(m_lbl_count);
        append(m_btn_prev); append(m_btn_next); append(m_btn_close);
    }
    void focus() { m_entry.grab_focus(); }
    void clear() { m_entry.set_text(""); m_lbl_count.set_text(""); }
    void set_count(int idx, int total) {
        if (total==0)   m_lbl_count.set_text("No results");
        else if (idx<0) m_lbl_count.set_text(itos(total)+" found");
        else            m_lbl_count.set_text(itos(idx+1)+" / "+itos(total));
    }
private:
    Gtk::Entry  m_entry;
    Gtk::Label  m_lbl_count;
    Gtk::Button m_btn_prev, m_btn_next, m_btn_close;
};

// ============================================================
//  BookmarkColumns
// ============================================================
class BookmarkColumns : public Gtk::TreeModel::ColumnRecord {
public:
    BookmarkColumns() { add(title); add(page); }
    Gtk::TreeModelColumn<Glib::ustring> title;
    Gtk::TreeModelColumn<int>           page;
};

// ============================================================
//  PdfPane  --  one complete self-contained tab content
//              (sidebar + scroll area + search bar)
// ============================================================
class PdfPane : public Gtk::Box {
public:
    std::string filepath;
    std::string filename;

    // Emitted when page/zoom changes (MainWindow updates toolbar)
    sigc::signal<void()> signal_state_changed;

    PdfPane() : Gtk::Box(Gtk::Orientation::VERTICAL) {
        build();
    }

    bool load(const std::string& path) {
        if (!m_view.load_document(path)) return false;
        filepath = path;
        auto slash = path.find_last_of("/\\");
        filename = (slash == std::string::npos) ? path : path.substr(slash+1);
        populate_outline();
        auto vadj = m_scroll.get_vadjustment();
        if (vadj) vadj->set_value(0);
        signal_state_changed.emit();
        return true;
    }

    bool   has_doc()      const { return m_view.has_doc(); }
    int    total_pages()  const { return m_view.total_pages(); }
    int    current_page() const { return m_view.current_page(); }
    double zoom()         const { return m_view.zoom(); }

    void zoom_in()    { m_view.zoom_in();    signal_state_changed.emit(); }
    void zoom_out()   { m_view.zoom_out();   signal_state_changed.emit(); }
    void zoom_100()   { m_view.zoom_100();   signal_state_changed.emit(); }
    void zoom_fit_w() { m_view.zoom_fit_w(); signal_state_changed.emit(); }
    void rotate_cw()  { m_view.rotate_cw();  signal_state_changed.emit(); }
    void rotate_ccw() { m_view.rotate_ccw(); signal_state_changed.emit(); }

    void go_prev() { int p=m_view.current_page()-1; if(p>=0) scroll_to_page(p); }
    void go_next() { int p=m_view.current_page()+1; if(p<m_view.total_pages()) scroll_to_page(p); }

    void toggle_sidebar() {
        m_sidebar_vis = !m_sidebar_vis; m_sidebar.set_visible(m_sidebar_vis);
    }
    void toggle_search() {
        m_search_vis = !m_search_vis; m_searchbar.set_visible(m_search_vis);
        if (m_search_vis) m_searchbar.focus(); else close_search();
    }
    void close_search() {
        m_search_vis = false; m_searchbar.set_visible(false);
        m_searchbar.clear(); m_view.clear_search();
    }
    bool search_visible() const { return m_search_vis; }

    // Scroll the viewport by delta pixels (for keyboard arrow keys)
    void scroll_by(double delta) {
        auto vadj = m_scroll.get_vadjustment(); if (!vadj) return;
        double cur = vadj->get_value();
        double lo  = vadj->get_lower();
        double hi  = vadj->get_upper() - vadj->get_page_size();
        vadj->set_value(std::max(lo, std::min(cur+delta, hi)));
    }
    void scroll_page(double sign) {
        auto vadj = m_scroll.get_vadjustment(); if (!vadj) return;
        double cur  = vadj->get_value();
        double lo   = vadj->get_lower();
        double hi   = vadj->get_upper() - vadj->get_page_size();
        double page = vadj->get_page_size();
        vadj->set_value(std::max(lo, std::min(cur+sign*page*0.9, hi)));
    }
    void scroll_home() { auto v=m_scroll.get_vadjustment(); if(v) v->set_value(v->get_lower()); }
    void scroll_end()  { auto v=m_scroll.get_vadjustment(); if(v) v->set_value(v->get_upper()-v->get_page_size()); }

private:
    MultiPageView       m_view;
    Gtk::ScrolledWindow m_scroll;
    SearchBar           m_searchbar;
    bool                m_search_vis = false;

    Gtk::Paned          m_paned      {Gtk::Orientation::HORIZONTAL};
    Gtk::Box            m_sidebar    {Gtk::Orientation::VERTICAL};
    Gtk::Label          m_sidebar_lbl;
    Gtk::ScrolledWindow m_tree_scroll;
    BookmarkColumns     m_bcols;
    Glib::RefPtr<Gtk::TreeStore> m_tree_store;
    Gtk::TreeView       m_tree_view;
    bool                m_sidebar_vis = true;

    void build() {
        // Sidebar
        m_sidebar.add_css_class("pdf-sidebar");
        m_sidebar.set_size_request(210, -1);
        m_sidebar_lbl.set_text("BOOKMARKS");
        m_sidebar_lbl.set_xalign(0.0);
        m_sidebar_lbl.add_css_class("sidebar-title");
        m_sidebar.append(m_sidebar_lbl);
        m_tree_store = Gtk::TreeStore::create(m_bcols);
        m_tree_view.set_model(m_tree_store);
        m_tree_view.set_headers_visible(false);
        m_tree_view.append_column("Title", m_bcols.title);
        m_tree_view.set_activate_on_single_click(true);
        m_tree_view.signal_row_activated().connect(
            [this](const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn*) {
                auto it = m_tree_store->get_iter(path);
                if (it) scroll_to_page((*it)[m_bcols.page]);
            });
        m_tree_scroll.set_child(m_tree_view);
        m_tree_scroll.set_expand(true);
        m_tree_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
        m_sidebar.append(m_tree_scroll);

        // Scroll area
        m_scroll.set_expand(true);
        m_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
        m_scroll.set_child(m_view);

        // Paned
        m_paned.set_start_child(m_sidebar);
        m_paned.set_end_child(m_scroll);
        m_paned.set_position(210);
        m_paned.set_shrink_start_child(false);
        m_paned.set_resize_start_child(false);
        m_paned.set_expand(true);

        // Search (hidden)
        m_searchbar.set_visible(false);
        m_searchbar.signal_search.connect([this](const std::string& text){
            int n = m_view.search_all(text);
            m_searchbar.set_count(m_view.hit_index(), n);
        });
        m_searchbar.signal_next.connect([this]{ m_view.search_next(); });
        m_searchbar.signal_prev.connect([this]{ m_view.search_prev(); });
        m_searchbar.signal_close.connect([this]{ close_search(); });

        // View signals → state change
        m_view.signal_page_changed().connect([this](int, int){ signal_state_changed.emit(); });
        m_view.signal_request_scroll().connect(sigc::mem_fun(*this, &PdfPane::scroll_to_page));

        // Scroll → notify view
        auto vadj = m_scroll.get_vadjustment();
        vadj->signal_value_changed().connect([this]{
            auto v = m_scroll.get_vadjustment(); if (!v) return;
            double sy = v->get_value(), vh = v->get_page_size();
            m_view.notify_scroll_position(sy, vh);
            double mid = sy + vh/2.0;
            int best=0; double best_d=1e9, n=m_view.total_pages();
            for (int i=0;i<n;++i) {
                double d = std::abs((double)m_view.page_y_offset(i)-mid);
                if (d<best_d) { best_d=d; best=i; }
            }
            m_view.notify_visible_page(best);
        });

        // Layout
        append(m_searchbar);
        append(m_paned);
    }

    void scroll_to_page(int idx) {
        if (!m_view.has_doc()) return;
        auto vadj = m_scroll.get_vadjustment();
        if (vadj) vadj->set_value((double)m_view.page_y_offset(idx));
        m_view.notify_visible_page(idx);
    }

    void populate_outline() {
        m_tree_store->clear();
        if (!m_view.has_doc()) return;
        PopplerIndexIter* iter = poppler_index_iter_new(m_view.doc());
        if (!iter) {
            auto row = *m_tree_store->append();
            row[m_bcols.title] = "(No bookmarks)"; row[m_bcols.page] = -1; return;
        }
        fill_outline(iter, Gtk::TreeModel::iterator());
        poppler_index_iter_free(iter);
        m_tree_view.expand_all();
    }
    void fill_outline(PopplerIndexIter* iter, Gtk::TreeModel::iterator parent) {
        do {
            PopplerAction* action = poppler_index_iter_get_action(iter);
            if (!action) continue;
            Glib::ustring title; int page = 0;
            if (action->type == POPPLER_ACTION_GOTO_DEST) {
                title = action->goto_dest.title
                      ? Glib::ustring(action->goto_dest.title) : "(untitled)";
                PopplerDest* dest = action->goto_dest.dest;
                if (dest) {
                    if (dest->type == POPPLER_DEST_NAMED) {
                        PopplerDest* nd = poppler_document_find_dest(m_view.doc(), dest->named_dest);
                        if (nd) { page = nd->page_num-1; poppler_dest_free(nd); }
                    } else { page = dest->page_num-1; }
                }
            } else if (action->type == POPPLER_ACTION_URI) {
                title = action->uri.title ? Glib::ustring(action->uri.title) : action->uri.uri;
            } else { poppler_action_free(action); continue; }
            poppler_action_free(action);
            Gtk::TreeModel::iterator row =
                parent ? m_tree_store->append(parent->children()) : m_tree_store->append();
            (*row)[m_bcols.title] = title; (*row)[m_bcols.page] = page;
            PopplerIndexIter* child = poppler_index_iter_get_child(iter);
            if (child) { fill_outline(child, row); poppler_index_iter_free(child); }
        } while (poppler_index_iter_next(iter));
    }
};

// ============================================================
//  MainWindow
// ============================================================
class MainWindow : public Gtk::ApplicationWindow {
public:
    MainWindow() {
        set_title("PDF Reader");
        set_default_size(1140, 780);

        auto css = Gtk::CssProvider::create();
        css->load_from_data(APP_CSS);
        Gtk::StyleContext::add_provider_for_display(
            Gdk::Display::get_default(), css,
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

        build_ui();
        connect_signals();
        setup_dnd();
    }

private:
    // Root layout
    Gtk::Box m_root {Gtk::Orientation::VERTICAL};

    // ── Toolbar ──────────────────────────────────────────
    Gtk::Box    m_toolbar   {Gtk::Orientation::HORIZONTAL, 2};
    Gtk::Button m_btn_sidebar, m_btn_open, m_btn_prev, m_btn_next;
    Gtk::SpinButton m_spin_page;
    Gtk::Label  m_lbl_total, m_lbl_zoom;
    Gtk::Button m_btn_zout, m_btn_zin, m_btn_zfit;
    Gtk::Button m_btn_rot_cw, m_btn_rot_ccw;
    Gtk::Button m_btn_search, m_btn_fs;

    // ── Notebook (tabs) ───────────────────────────────────
    Gtk::Notebook m_notebook;

    // ── Status bar ────────────────────────────────────────
    Gtk::Box  m_statusbar {Gtk::Orientation::HORIZONTAL};
    Gtk::Label m_lbl_path, m_lbl_pagenum;

    bool m_fullscreen    = false;

    // ── Toolbar helpers ───────────────────────────────────
    void tb_btn(Gtk::Button& b, const char* ic, const char* tip, int mw=34) {
        b.set_label(ic); b.set_tooltip_text(tip); b.set_size_request(mw,34);
        m_toolbar.append(b);
    }
    void tb_sep() {
        auto* s = Gtk::make_managed<Gtk::Box>();
        s->add_css_class("sep"); s->set_size_request(1,22); m_toolbar.append(*s);
    }
    void tb_spacer() {
        auto* s = Gtk::make_managed<Gtk::Box>(); s->set_hexpand(true); m_toolbar.append(*s);
    }

    // ── build_ui ──────────────────────────────────────────
    void build_ui() {
        // Toolbar
        m_toolbar.add_css_class("pdf-toolbar");
        tb_btn(m_btn_sidebar, "☰",  "Toggle bookmarks  B");
        tb_sep();
        tb_btn(m_btn_open,    "📂", "Open PDF  Ctrl+O", 38);
        tb_sep();
        tb_btn(m_btn_prev, "‹", "Previous page  ←");
        m_spin_page.set_range(1,9999); m_spin_page.set_increments(1,10);
        m_spin_page.set_value(1); m_spin_page.set_numeric(true);
        m_spin_page.add_css_class("page-spin");
        m_spin_page.set_tooltip_text("Jump to page"); m_toolbar.append(m_spin_page);
        m_lbl_total.set_text("/ —"); m_lbl_total.add_css_class("page-label");
        m_lbl_total.set_margin_start(3); m_lbl_total.set_margin_end(3);
        m_toolbar.append(m_lbl_total);
        tb_btn(m_btn_next, "›", "Next page  →");
        tb_sep();
        tb_btn(m_btn_zout, "－", "Zoom out  Ctrl+−");
        m_lbl_zoom.set_text("100%"); m_lbl_zoom.add_css_class("zoom-label");
        m_lbl_zoom.set_xalign(0.5); m_toolbar.append(m_lbl_zoom);
        tb_btn(m_btn_zin,  "＋", "Zoom in  Ctrl++");
        tb_btn(m_btn_zfit, "↔",  "Fit to width  Ctrl+Shift+W", 30);
        tb_sep();
        tb_btn(m_btn_rot_ccw, "↺", "Rotate left  Shift+R");
        tb_btn(m_btn_rot_cw,  "↻", "Rotate right  R");
        tb_sep();
        tb_spacer();
        tb_btn(m_btn_search, "🔍", "Find text  Ctrl+F");
        tb_btn(m_btn_fs,     "⛶",  "Full screen  F11");

        // Notebook
        m_notebook.set_scrollable(true);
        m_notebook.set_show_border(false);
        m_notebook.set_expand(true);
        // "+" button to open a new file
        auto* add_btn = Gtk::make_managed<Gtk::Button>("+");
        add_btn->set_tooltip_text("Open new PDF  Ctrl+O");
        add_btn->set_has_frame(false);
        add_btn->signal_clicked().connect([this]{ on_open(); });
        m_notebook.set_action_widget(add_btn, Gtk::PackType::END);

        // Status bar
        m_statusbar.add_css_class("pdf-statusbar");
        m_lbl_path.set_xalign(0.0); m_lbl_path.set_hexpand(true);
        m_lbl_path.set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        m_lbl_path.set_text("Drag a PDF here or click 📂 to open");
        m_lbl_pagenum.set_xalign(1.0);
        m_statusbar.append(m_lbl_path); m_statusbar.append(m_lbl_pagenum);

        // Keyboard shortcut hint label in notebook when empty
        m_root.append(m_toolbar);
        m_root.append(m_notebook);
        m_root.append(m_statusbar);
        set_child(m_root);

        update_ui_state();
    }

    // ── Tab management ────────────────────────────────────

    // Build the tab label widget: "filename.pdf  ✕"
    Gtk::Widget* make_tab_label(PdfPane* pane) {
        auto* box  = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
        auto* lbl  = Gtk::make_managed<Gtk::Label>();
        lbl->set_max_width_chars(18);
        lbl->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        lbl->set_text(pane->filename);
        auto* btn  = Gtk::make_managed<Gtk::Button>();
        btn->set_label("✕");
        btn->add_css_class("tab-close");
        btn->set_tooltip_text("Close tab  Ctrl+W");
        btn->signal_clicked().connect([this, pane]{ close_pane(pane); });
        box->append(*lbl);
        box->append(*btn);
        box->set_margin_top(1); box->set_margin_bottom(1);
        return box;
    }

    // Open a path in a new tab (or focus existing tab for same path)
    void open_in_tab(const std::string& path) {
        // Check if already open
        int n = m_notebook.get_n_pages();
        for (int i = 0; i < n; ++i) {
            auto* pane = dynamic_cast<PdfPane*>(m_notebook.get_nth_page(i));
            if (pane && pane->filepath == path) {
                m_notebook.set_current_page(i); return;
            }
        }

        auto* pane = Gtk::make_managed<PdfPane>();
        if (!pane->load(path)) {
            show_error("Cannot open file:\n" + path); return;
        }

        pane->signal_state_changed.connect([this]{ on_state_changed(); });

        auto* label = make_tab_label(pane);
        int idx = m_notebook.append_page(*pane, *label);
        m_notebook.set_tab_reorderable(*pane, true);
        m_notebook.set_current_page(idx);
        update_ui_state();
    }

    void close_pane(PdfPane* pane) {
        int n = m_notebook.get_n_pages();
        for (int i = 0; i < n; ++i) {
            if (m_notebook.get_nth_page(i) == pane) {
                m_notebook.remove_page(i);
                update_ui_state();
                return;
            }
        }
    }

    PdfPane* current_pane() {
        int idx = m_notebook.get_current_page();
        if (idx < 0) return nullptr;
        return dynamic_cast<PdfPane*>(m_notebook.get_nth_page(idx));
    }

    // ── connect_signals ───────────────────────────────────
    void connect_signals() {
        m_btn_open.signal_clicked().connect([this]{ on_open(); });
        m_btn_prev.signal_clicked().connect([this]{ if(auto*p=current_pane()) p->go_prev(); });
        m_btn_next.signal_clicked().connect([this]{ if(auto*p=current_pane()) p->go_next(); });
        m_btn_zin.signal_clicked().connect([this]{ if(auto*p=current_pane()) p->zoom_in(); });
        m_btn_zout.signal_clicked().connect([this]{ if(auto*p=current_pane()) p->zoom_out(); });
        m_btn_zfit.signal_clicked().connect([this]{ if(auto*p=current_pane()) p->zoom_fit_w(); });
        m_btn_rot_cw.signal_clicked().connect([this]{ if(auto*p=current_pane()) p->rotate_cw(); });
        m_btn_rot_ccw.signal_clicked().connect([this]{ if(auto*p=current_pane()) p->rotate_ccw(); });
        m_btn_sidebar.signal_clicked().connect([this]{ if(auto*p=current_pane()) p->toggle_sidebar(); });
        m_btn_search.signal_clicked().connect([this]{ if(auto*p=current_pane()) p->toggle_search(); });
        m_btn_fs.signal_clicked().connect([this]{ toggle_fullscreen(); });

        m_spin_page.signal_value_changed().connect([this]{
            auto* p = current_pane(); if (!p || !p->has_doc()) return;
            // spin page is 1-based; we need to call via PdfPane
            // PdfPane exposes scroll via go_prev/next, here we use direct scroll:
            // (future: expose scroll_to_page on PdfPane)
        });

        m_notebook.signal_switch_page().connect([this](Gtk::Widget*, guint){
            on_state_changed();
        });

        auto key = Gtk::EventControllerKey::create();
        key->signal_key_pressed().connect(
            sigc::mem_fun(*this, &MainWindow::on_key), false);
        add_controller(key);
    }

    // ── Drag-and-drop ─────────────────────────────────────
    void setup_dnd() {
        auto drop = Gtk::DropTarget::create(GDK_TYPE_FILE_LIST, Gdk::DragAction::COPY);

        // signal_drop requires the second bool (after=false) argument
        drop->signal_drop().connect(
            [this](const Glib::ValueBase& val, double, double) -> bool {
                const GValue* gv = val.gobj();
                if (!G_VALUE_HOLDS(gv, GDK_TYPE_FILE_LIST)) return false;
                auto* list = static_cast<GSList*>(g_value_get_boxed(gv));
                bool opened = false;
                for (auto* l = list; l; l = l->next) {
                    auto* gfile = G_FILE(l->data);
                    if (!gfile) continue;
                    gchar* raw = g_file_get_path(gfile);
                    if (!raw) continue;
                    std::string p(raw); g_free(raw);
                    // case-insensitive .pdf check
                    std::string ext = p.size() > 4 ? p.substr(p.size() - 4) : "";
                    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
                    if (ext == ".pdf") { open_in_tab(p); opened = true; }
                }
                remove_css_class("drop-highlight");
                return opened;
            }, false);

        drop->signal_enter().connect(
            [this](double, double) -> Gdk::DragAction {
                add_css_class("drop-highlight");
                return Gdk::DragAction::COPY;
            }, false);

        drop->signal_leave().connect([this]{
            remove_css_class("drop-highlight");
        });

        add_controller(drop);
    }

    // ── Keyboard shortcuts ────────────────────────────────
    bool on_key(guint key, guint, Gdk::ModifierType mod) {
        bool ctrl  = (mod & Gdk::ModifierType::CONTROL_MASK)  == Gdk::ModifierType::CONTROL_MASK;
        bool shift = (mod & Gdk::ModifierType::SHIFT_MASK)    == Gdk::ModifierType::SHIFT_MASK;
        auto* pane = current_pane();

        // ── Global shortcuts (no doc needed) ──────────────
        if (ctrl && key == GDK_KEY_o)  { on_open(); return true; }
        if (ctrl && key == GDK_KEY_w)  { if (pane) close_pane(pane); return true; }
        if (key  == GDK_KEY_F11)       { toggle_fullscreen(); return true; }

        // Tab navigation
        if (ctrl && key == GDK_KEY_Tab) {
            int n = m_notebook.get_n_pages();
            if (n > 1) m_notebook.set_current_page((m_notebook.get_current_page()+1) % n);
            return true;
        }
        if (ctrl && shift && key == GDK_KEY_Tab) {
            int n = m_notebook.get_n_pages();
            if (n > 1) m_notebook.set_current_page((m_notebook.get_current_page()-1+n) % n);
            return true;
        }
        // Tab by number Ctrl+1…9
        if (ctrl && key >= GDK_KEY_1 && key <= GDK_KEY_9) {
            int idx = (int)(key - GDK_KEY_1);
            if (idx < m_notebook.get_n_pages()) m_notebook.set_current_page(idx);
            return true;
        }

        if (!pane) return false;

        // ── Escape ────────────────────────────────────────
        if (key == GDK_KEY_Escape) {
            if (m_fullscreen)          { toggle_fullscreen();    return true; }
            if (pane->search_visible()) { pane->close_search();  return true; }
        }

        // ── Sidebar / Search ──────────────────────────────
        if (!ctrl && (key==GDK_KEY_b||key==GDK_KEY_B)) { pane->toggle_sidebar(); return true; }
        if (ctrl && key==GDK_KEY_f)  { pane->toggle_search(); return true; }

        // ── Scroll (arrow keys) ───────────────────────────
        if (key==GDK_KEY_Down)      { pane->scroll_by(+SCROLL_STEP); return true; }
        if (key==GDK_KEY_Up)        { pane->scroll_by(-SCROLL_STEP); return true; }
        if (key==GDK_KEY_Page_Down) { pane->scroll_page(+1); return true; }
        if (key==GDK_KEY_Page_Up)   { pane->scroll_page(-1); return true; }
        if (key==GDK_KEY_Home)      { pane->scroll_home(); return true; }
        if (key==GDK_KEY_End)       { pane->scroll_end();  return true; }

        // ── Zoom ──────────────────────────────────────────
        if (ctrl && (key==GDK_KEY_plus||key==GDK_KEY_equal))
            { pane->zoom_in();    return true; }
        if (ctrl && key==GDK_KEY_minus)
            { pane->zoom_out();   return true; }
        if (ctrl && key==GDK_KEY_0)
            { pane->zoom_100();   return true; }
        if (ctrl && shift && (key==GDK_KEY_w||key==GDK_KEY_W))
            { pane->zoom_fit_w(); return true; }

        // ── Rotate ────────────────────────────────────────
        if (!ctrl && (key==GDK_KEY_r))   { pane->rotate_cw();  return true; }
        if (!ctrl && shift && (key==GDK_KEY_R)) { pane->rotate_ccw(); return true; }

        return false;
    }

    // ── Open file dialog ──────────────────────────────────
    void on_open() {
        auto dlg = Gtk::FileDialog::create();
        dlg->set_title("Open PDF");
        auto filter = Gtk::FileFilter::create();
        filter->set_name("PDF Files");
        filter->add_mime_type("application/pdf");
        filter->add_pattern("*.pdf");
        auto filters = Gio::ListStore<Gtk::FileFilter>::create();
        filters->append(filter); dlg->set_filters(filters);

        // Allow selecting multiple files at once
        dlg->open_multiple(*this, [this, dlg](Glib::RefPtr<Gio::AsyncResult>& res){
            try {
                auto files = dlg->open_multiple_finish(res);
                for (auto& file : files) {
                    if (file) open_in_tab(file->get_path());
                }
            } catch (const Glib::Error&) {}
        });
    }

    // ── State change (page / zoom update) ─────────────────
    void on_state_changed() {
        auto* p = current_pane();
        if (!p || !p->has_doc()) { update_ui_state(); return; }

        int cur = p->current_page(), tot = p->total_pages();
        m_spin_page.set_range(1, tot>0?tot:1);
        { bool was=m_spin_page.get_sensitive();
          m_spin_page.set_sensitive(false); m_spin_page.set_value(cur+1);
          m_spin_page.set_sensitive(was); }
        m_lbl_total.set_text("/ "+itos(tot));
        m_lbl_pagenum.set_text(itos(cur+1)+" / "+itos(tot));
        m_lbl_zoom.set_text(itos((int)std::round(p->zoom()*100.0))+"%");
        m_lbl_path.set_text(p->filepath);
        set_title(p->filename + " — PDF Reader");

        m_btn_prev.set_sensitive(cur > 0);
        m_btn_next.set_sensitive(cur < tot-1);
        update_ui_state();
    }

    void toggle_fullscreen() {
        m_fullscreen = !m_fullscreen;
        if (m_fullscreen) fullscreen(); else unfullscreen();
        m_btn_fs.set_label(m_fullscreen ? "⊠" : "⛶");
    }

    void update_ui_state() {
        auto* p  = current_pane();
        bool has = p && p->has_doc();

        m_btn_prev.set_sensitive(has && p->current_page()>0);
        m_btn_next.set_sensitive(has && p->current_page()<p->total_pages()-1);
        m_spin_page.set_sensitive(has);
        m_btn_zin.set_sensitive(has); m_btn_zout.set_sensitive(has);
        m_btn_zfit.set_sensitive(has);
        m_btn_rot_cw.set_sensitive(has); m_btn_rot_ccw.set_sensitive(has);
        m_btn_search.set_sensitive(has);

        if (!has) {
            m_lbl_total.set_text("/ —"); m_lbl_zoom.set_text("—");
            m_lbl_pagenum.set_text("");
            if (m_notebook.get_n_pages()==0) {
                set_title("PDF Reader");
                m_lbl_path.set_text("Drag a PDF here or click 📂 to open");
            }
        }
    }

    void show_error(const std::string& msg) {
        Gtk::AlertDialog::create(msg)->show(*this);
    }
};

// ============================================================
//  main
// ============================================================
int main(int argc, char* argv[]) {
    auto app = Gtk::Application::create("org.example.pdfreader");
    return app->make_window_and_run<MainWindow>(argc, argv);
}
