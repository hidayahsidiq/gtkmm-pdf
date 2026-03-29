// ============================================================
//  PDF Reader  --  gtkmm-4.0 + poppler-glib
//
//  Optimizations:
//    - Background thread rendering (non-blocking UI)
//    - Debounced zoom/rotate (coalesces rapid clicks)
//    - Viewport culling (only renders visible pages)
//    - Cached page sizes (no redundant poppler calls)
//    - Incremental rendering with idle dispatch
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
#include <functional>

static std::string itos(int v) { return std::to_string(v); }

// ============================================================
//  CSS
// ============================================================
static const char* APP_CSS = R"CSS(

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
.pdf-toolbar button:hover {
    background-color: alpha(@accent_bg_color, 0.15);
}
.pdf-toolbar > button:active,
.pdf-toolbar button:active {
    background-color: alpha(@accent_bg_color, 0.28);
}
.pdf-toolbar > button:disabled,
.pdf-toolbar button:disabled {
    opacity: 0.35;
}
.pdf-toolbar .sep {
    min-width: 1px;
    min-height: 20px;
    background-color: alpha(@borders, 0.6);
    margin: 0 5px;
}
.page-spin {
    min-width: 46px;
    max-width: 52px;
    min-height: 28px;
}
.page-label {
    font-size: 0.88em;
    color: #000000;
    padding: 0 2px;
}
.zoom-label {
    font-size: 0.85em;
    color: #000000;
    min-width: 44px;
    padding: 0 2px;
}
.search-bar-box {
    background-color: @headerbar_bg_color;
    border-bottom: 1px solid alpha(@borders, 0.8);
    padding: 4px 10px;
}
.search-bar-box entry {
    min-width: 200px;
}
.search-count {
    font-size: 0.82em;
    color: #000000;
    min-width: 60px;
    padding: 0 6px;
}
.pdf-sidebar {
    background-color: @sidebar_bg_color;
    border-right: 1px solid alpha(@borders, 0.7);
}
.sidebar-title {
    font-weight: bold;
    font-size: 0.76em;
    letter-spacing: 0.07em;
    color: #000000;
    padding: 10px 12px 5px 12px;
}
.pdf-statusbar {
    background-color: @headerbar_bg_color;
    border-top: 1px solid alpha(@borders, 0.6);
    padding: 0 12px;
    min-height: 22px;
    font-size: 0.76em;
    color: #000000;
}

)CSS";

// ============================================================
//  PageSize cache  --  width/height in PDF points
// ============================================================
struct PageSize { double w = 0, h = 0; };

// ============================================================
//  RenderedPage  --  owns one Cairo surface + Pixbuf
// ============================================================
struct RenderedPage {
    int                        index    = -1;
    double                     at_zoom  = 0;
    int                        at_rot   = -1;
    cairo_surface_t*           surf     = nullptr;
    Glib::RefPtr<Gdk::Pixbuf>  pixbuf;
    int                        w        = 0;
    int                        h        = 0;

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
            index   = o.index;
            at_zoom = o.at_zoom;
            at_rot  = o.at_rot;
            surf    = o.surf;  o.surf = nullptr;
            pixbuf  = std::move(o.pixbuf);
            w = o.w; h = o.h;
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

    // Render on background thread. Caller must NOT hold any GTK lock.
    bool render(PopplerPage* page, double scale, int rotation, const PageSize& ps) {
        reset();
        bool rotated = (rotation == 90 || rotation == 270);
        double rw = rotated ? ps.h : ps.w;
        double rh = rotated ? ps.w : ps.h;
        w = std::max(1, (int)(rw * scale));
        h = std::max(1, (int)(rh * scale));

        surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(surf); surf = nullptr; return false;
        }
        cairo_t* cr = cairo_create(surf);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_paint(cr);
        cairo_scale(cr, scale, scale);
        switch (rotation) {
            case 90:  cairo_translate(cr, ps.h, 0);      cairo_rotate(cr,  M_PI / 2.0); break;
            case 180: cairo_translate(cr, ps.w, ps.h);   cairo_rotate(cr,  M_PI);       break;
            case 270: cairo_translate(cr, 0,    ps.w);   cairo_rotate(cr, -M_PI / 2.0); break;
            default: break;
        }
        poppler_page_render(page, cr);
        cairo_destroy(cr);
        at_zoom = scale; at_rot = rotation;
        return true;
    }

    // Must be called on the GTK main thread after render().
    void build_pixbuf() {
        if (!surf) return;
        pixbuf = Gdk::Pixbuf::create_from_data(
            cairo_image_surface_get_data(surf),
            Gdk::Colorspace::RGB, true, 8,
            w, h, cairo_image_surface_get_stride(surf));
    }
};

// ============================================================
//  SearchHit
// ============================================================
struct SearchHit { int page_idx; PopplerRectangle rect; };

// ============================================================
//  RenderJob
// ============================================================
struct RenderJob {
    int      page_idx = -1;
    double   zoom     = 1.0;
    int      rotation = 0;
    uint64_t gen      = 0;
};

static const int    PAGE_GAP    = 14;
static const double SCROLL_STEP = 80.0;

// ============================================================
//  MultiPageView
// ============================================================
class MultiPageView : public Gtk::DrawingArea {
public:
    MultiPageView() {
        set_expand(true);
        set_draw_func(sigc::mem_fun(*this, &MultiPageView::on_draw));
    }
    ~MultiPageView() override { stop_worker(); free_doc(); }

    // ── Load ──────────────────────────────────────────────
    bool load_document(const std::string& path) {
        cancel_pending();
        free_doc();

        GError* err = nullptr;
        gchar*  uri = g_filename_to_uri(path.c_str(), nullptr, &err);
        if (!uri) { if (err) g_error_free(err); return false; }
        m_doc = poppler_document_new_from_file(uri, nullptr, &err);
        g_free(uri);
        if (!m_doc) { if (err) g_error_free(err); return false; }

        int n = poppler_document_get_n_pages(m_doc);
        m_page_sizes.resize(n);
        m_pages.resize(n);

        for (int i = 0; i < n; ++i) {
            PopplerPage* pg = poppler_document_get_page(m_doc, i);
            if (pg) {
                poppler_page_get_size(pg, &m_page_sizes[i].w, &m_page_sizes[i].h);
                g_object_unref(pg);
            }
        }
        m_zoom = 1.0; m_rotation = 0;
        recompute_layout();
        enqueue_visible_pages();
        return true;
    }

    bool has_doc()      const { return m_doc != nullptr; }
    int  total_pages()  const { return m_doc ? (int)m_page_sizes.size() : 0; }
    int  current_page() const { return m_visible_page; }
    double zoom()       const { return m_zoom; }
    PopplerDocument* doc()    const { return m_doc; }

    // ── Zoom / Rotate (debounced) ──────────────────────────
    void zoom_in()    { request_zoom(m_zoom * 1.20); }
    void zoom_out()   { request_zoom(m_zoom / 1.20); }
    void zoom_100()   { request_zoom(1.0); }
    void zoom_fit_w() {
        if (!m_doc || m_page_sizes.empty()) return;
        bool rotated = (m_rotation == 90 || m_rotation == 270);
        double ref_w = rotated ? m_page_sizes[0].h : m_page_sizes[0].w;
        int aw = get_width(); if (aw < 10) aw = 800;
        request_zoom((aw - 40.0) / ref_w);
    }
    void rotate_cw()  { request_rotate((m_rotation + 90)  % 360); }
    void rotate_ccw() { request_rotate((m_rotation + 270) % 360); }

    // ── Navigation ────────────────────────────────────────
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
        m_viewport_top = top;
        m_viewport_h   = view_h;
        enqueue_visible_pages();
    }

    // ── Search ────────────────────────────────────────────
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
            g_list_free_full(list,
                reinterpret_cast<GDestroyNotify>(poppler_rectangle_free));
            g_object_unref(pg);
        }
        if (!m_hits.empty()) { m_hit_idx = 0; scroll_to_page(m_hits[0].page_idx); }
        queue_draw();
        return (int)m_hits.size();
    }
    void search_next() {
        if (m_hits.empty()) return;
        m_hit_idx = (m_hit_idx + 1) % (int)m_hits.size();
        scroll_to_page(m_hits[m_hit_idx].page_idx);
        queue_draw();
        m_sig_search_changed.emit(m_hit_idx, (int)m_hits.size());
    }
    void search_prev() {
        if (m_hits.empty()) return;
        m_hit_idx = (m_hit_idx - 1 + (int)m_hits.size()) % (int)m_hits.size();
        scroll_to_page(m_hits[m_hit_idx].page_idx);
        queue_draw();
        m_sig_search_changed.emit(m_hit_idx, (int)m_hits.size());
    }
    void clear_search() {
        m_hits.clear(); m_hit_idx = -1; m_search_text.clear(); queue_draw();
    }
    int hit_count() const { return (int)m_hits.size(); }
    int hit_index() const { return m_hit_idx; }

    // ── Signals ───────────────────────────────────────────
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
    int                       m_total_h      = 0;
    int                       m_max_w        = 0;

    double                    m_zoom         = 1.0;
    int                       m_rotation     = 0;
    int                       m_visible_page = 0;

    double                    m_viewport_top = 0;
    double                    m_viewport_h   = 600;

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

    // Worker thread
    std::thread             m_worker;
    std::mutex              m_queue_mutex;
    std::condition_variable m_queue_cv;
    std::vector<RenderJob>  m_queue;
    std::atomic<bool>       m_worker_stop{false};
    std::atomic<uint64_t>   m_gen{0};

    // ── Worker ────────────────────────────────────────────
    void start_worker() {
        if (m_worker.joinable()) return;
        m_worker_stop = false;
        m_worker = std::thread([this]{ worker_loop(); });
    }
    void stop_worker() {
        m_worker_stop = true;
        m_queue_cv.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }
    void cancel_pending() {
        ++m_gen;
        std::lock_guard<std::mutex> lk(m_queue_mutex);
        m_queue.clear();
    }
    void push_job(int page_idx, bool high_priority) {
        RenderJob job;
        job.page_idx = page_idx;
        job.zoom     = m_zoom;
        job.rotation = m_rotation;
        job.gen      = m_gen.load();
        {
            std::lock_guard<std::mutex> lk(m_queue_mutex);
            for (auto& j : m_queue)
                if (j.page_idx == page_idx && j.gen == job.gen) return;
            if (high_priority)
                m_queue.insert(m_queue.begin(), job);
            else
                m_queue.push_back(job);
        }
        m_queue_cv.notify_one();
    }
    void worker_loop() {
        while (!m_worker_stop) {
            RenderJob job;
            {
                std::unique_lock<std::mutex> lk(m_queue_mutex);
                m_queue_cv.wait(lk, [this]{
                    return m_worker_stop || !m_queue.empty();
                });
                if (m_worker_stop) break;
                job = m_queue.front();
                m_queue.erase(m_queue.begin());
            }

            if (job.gen != m_gen.load()) continue;
            if (!m_doc) continue;
            if (job.page_idx < 0 || job.page_idx >= (int)m_page_sizes.size()) continue;

            {
                std::lock_guard<std::mutex> lk(m_pages_mutex);
                if (m_pages[job.page_idx].valid_for(job.zoom, job.rotation)) continue;
            }

            PopplerPage* pg = poppler_document_get_page(m_doc, job.page_idx);
            if (!pg) continue;

            auto* heap_rp = new RenderedPage();
            heap_rp->index = job.page_idx;
            bool ok = heap_rp->render(pg, job.zoom, job.rotation,
                                      m_page_sizes[job.page_idx]);
            g_object_unref(pg);

            if (!ok || job.gen != m_gen.load()) { delete heap_rp; continue; }

            int      idx = job.page_idx;
            uint64_t gen = job.gen;

            Glib::signal_idle().connect_once([this, heap_rp, idx, gen]() {
                std::unique_ptr<RenderedPage> owned(heap_rp);
                if (gen != m_gen.load()) return;
                owned->build_pixbuf();
                {
                    std::lock_guard<std::mutex> lk(m_pages_mutex);
                    m_pages[idx] = std::move(*owned);
                }
                queue_draw();
            });
        }
    }

    // ── Layout ────────────────────────────────────────────
    void recompute_layout() {
        int n = (int)m_page_sizes.size();
        m_page_y.resize(n);
        m_total_h = PAGE_GAP;
        m_max_w   = 0;
        bool rotated = (m_rotation == 90 || m_rotation == 270);
        for (int i = 0; i < n; ++i) {
            m_page_y[i] = m_total_h;
            double pw = rotated ? m_page_sizes[i].h : m_page_sizes[i].w;
            double ph = rotated ? m_page_sizes[i].w : m_page_sizes[i].h;
            int pw_px = std::max(1, (int)(pw * m_zoom));
            int ph_px = std::max(1, (int)(ph * m_zoom));
            m_total_h += ph_px + PAGE_GAP;
            m_max_w    = std::max(m_max_w, pw_px);
        }
        set_size_request(m_max_w + 40, m_total_h);
        m_sig_page_changed.emit(m_visible_page, n);
    }

    void enqueue_visible_pages() {
        if (!m_doc) return;
        start_worker();
        int n = (int)m_page_sizes.size();
        bool rotated = (m_rotation == 90 || m_rotation == 270);
        // Render one screen above/below viewport as prefetch
        double top = m_viewport_top - m_viewport_h;
        double bot = m_viewport_top + m_viewport_h * 2.0;
        for (int i = 0; i < n; ++i) {
            int page_top = m_page_y[i];
            double ph = rotated ? m_page_sizes[i].w : m_page_sizes[i].h;
            int page_bot = page_top + (int)(ph * m_zoom);
            if (page_bot < top || page_top > bot) continue;
            {
                std::lock_guard<std::mutex> lk(m_pages_mutex);
                if (m_pages[i].valid_for(m_zoom, m_rotation)) continue;
            }
            bool visible = (page_bot >= m_viewport_top &&
                            page_top <= m_viewport_top + m_viewport_h);
            push_job(i, visible);
        }
    }

    // ── Debounce ──────────────────────────────────────────
    void request_zoom(double z) {
        m_pending_zoom = std::max(0.08, std::min(z, 8.0));
        m_pending_rot  = m_rotation;
        arm_debounce();
    }
    void request_rotate(int rot) {
        m_pending_rot  = rot;
        m_pending_zoom = m_zoom;
        arm_debounce();
    }
    void arm_debounce() {
        m_debounce_conn.disconnect();
        // Apply layout change immediately so the UI feels responsive,
        // then kick off background re-render after 80 ms of inactivity.
        m_zoom     = m_pending_zoom;
        m_rotation = m_pending_rot;
        recompute_layout();
        queue_draw();

        m_debounce_conn = Glib::signal_timeout().connect([this]() -> bool {
            cancel_pending();
            {
                std::lock_guard<std::mutex> lk(m_pages_mutex);
                for (auto& p : m_pages) p.reset();
            }
            enqueue_visible_pages();
            return false;
        }, 80);
    }

    void free_doc() {
        m_debounce_conn.disconnect();
        {
            std::lock_guard<std::mutex> lk(m_pages_mutex);
            m_pages.clear();
        }
        m_page_sizes.clear(); m_page_y.clear();
        m_hits.clear(); m_hit_idx = -1; m_visible_page = 0;
        m_total_h = 0; m_max_w = 0;
        if (m_doc) { g_object_unref(m_doc); m_doc = nullptr; }
    }

    // ── Draw ──────────────────────────────────────────────
    void on_draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int /*h*/) {
        cr->set_source_rgb(0.17, 0.17, 0.17);
        cr->paint();

        std::lock_guard<std::mutex> lk(m_pages_mutex);
        int n = (int)m_pages.size();
        if (n == 0) return;

        bool rotated = (m_rotation == 90 || m_rotation == 270);
        for (int i = 0; i < n; ++i) {
            int y = m_page_y[i];
            double pw_pt = rotated ? m_page_sizes[i].h : m_page_sizes[i].w;
            double ph_pt = rotated ? m_page_sizes[i].w : m_page_sizes[i].h;
            int pw_px = std::max(1, (int)(pw_pt * m_zoom));
            int ph_px = std::max(1, (int)(ph_pt * m_zoom));
            int ox = std::max(0, (w - pw_px) / 2);

            // Drop shadow
            cr->set_source_rgba(0, 0, 0, 0.28);
            cr->rectangle(ox + 3, y + 3, pw_px, ph_px);
            cr->fill();

            if (m_pages[i].pixbuf) {
                Gdk::Cairo::set_source_pixbuf(cr, m_pages[i].pixbuf, ox, y);
                cr->paint();
            } else {
                // Placeholder while background thread renders
                cr->set_source_rgb(0.96, 0.96, 0.96);
                cr->rectangle(ox, y, pw_px, ph_px);
                cr->fill();
                cr->set_source_rgba(0.5, 0.5, 0.5, 0.7);
                cr->select_font_face("Sans",
                    Cairo::ToyFontFace::Slant::NORMAL,
                    Cairo::ToyFontFace::Weight::NORMAL);
                cr->set_font_size(13.0);
                Cairo::TextExtents te;
                cr->get_text_extents("Rendering…", te);
                cr->move_to(ox + (pw_px - te.width)  / 2.0,
                            y  + (ph_px + te.height) / 2.0);
                cr->show_text("Rendering…");
            }

            draw_search_highlights(cr, i, ox, y);
        }
    }

    void draw_search_highlights(const Cairo::RefPtr<Cairo::Context>& cr,
                                int page_i, int ox, int oy)
    {
        if (m_hits.empty()) return;
        double ph_pt = m_page_sizes[page_i].h;
        for (int j = 0; j < (int)m_hits.size(); ++j) {
            const auto& h = m_hits[j];
            if (h.page_idx != page_i) continue;
            double s  = m_zoom;
            double x1 = h.rect.x1 * s + ox;
            double y1 = (ph_pt - h.rect.y2) * s + oy;
            double bw = (h.rect.x2 - h.rect.x1) * s;
            double bh = (h.rect.y2 - h.rect.y1) * s;
            cr->set_source_rgba(0.98, 0.82, 0.10,
                                (j == m_hit_idx) ? 0.82 : 0.35);
            cr->rectangle(x1, y1, bw, bh);
            cr->fill();
        }
    }
};

// ============================================================
//  SearchBar
// ============================================================
class SearchBar : public Gtk::Box {
public:
    sigc::signal<void(const std::string&)> signal_search;
    sigc::signal<void()>                   signal_next;
    sigc::signal<void()>                   signal_prev;
    sigc::signal<void()>                   signal_close;

    SearchBar() : Gtk::Box(Gtk::Orientation::HORIZONTAL, 4) {
        add_css_class("search-bar-box");
        set_margin_start(4); set_margin_end(4);
        auto icon = Gtk::make_managed<Gtk::Label>("🔍");
        icon->set_margin_end(2);
        append(*icon);

        m_entry.set_placeholder_text("Search in document…");
        m_entry.set_hexpand(true);
        m_entry.signal_changed().connect([this]{
            signal_search.emit(m_entry.get_text()); });
        m_entry.signal_activate().connect([this]{ signal_next.emit(); });

        auto key = Gtk::EventControllerKey::create();
        key->signal_key_pressed().connect(
            [this](guint k, guint, Gdk::ModifierType) -> bool {
                if (k == GDK_KEY_Escape)
                    { signal_close.emit(); return true; }
                if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter)
                    { signal_next.emit(); return true; }
                return false;
            }, false);
        m_entry.add_controller(key);

        m_lbl_count.add_css_class("search-count");
        m_btn_prev.set_label("▲");
        m_btn_prev.set_tooltip_text("Previous match");
        m_btn_prev.signal_clicked().connect([this]{ signal_prev.emit(); });
        m_btn_next.set_label("▼");
        m_btn_next.set_tooltip_text("Next match");
        m_btn_next.signal_clicked().connect([this]{ signal_next.emit(); });
        m_btn_close.set_label("✕");
        m_btn_close.set_tooltip_text("Close  Esc");
        m_btn_close.signal_clicked().connect([this]{ signal_close.emit(); });

        append(m_entry); append(m_lbl_count);
        append(m_btn_prev); append(m_btn_next); append(m_btn_close);
    }
    void focus() { m_entry.grab_focus(); }
    void clear() { m_entry.set_text(""); m_lbl_count.set_text(""); }
    void set_count(int idx, int total) {
        if (total == 0)   m_lbl_count.set_text("No results");
        else if (idx < 0) m_lbl_count.set_text(itos(total) + " found");
        else              m_lbl_count.set_text(itos(idx+1) + " / " + itos(total));
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
//  MainWindow
// ============================================================
class MainWindow : public Gtk::ApplicationWindow {
public:
    MainWindow() {
        set_title("PDF Reader");
        set_default_size(1100, 760);
        auto css = Gtk::CssProvider::create();
        css->load_from_data(APP_CSS);
        Gtk::StyleContext::add_provider_for_display(
            Gdk::Display::get_default(), css,
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        build_ui();
        connect_signals();
    }

private:
    Gtk::Box            m_root      {Gtk::Orientation::VERTICAL};
    Gtk::Box            m_toolbar   {Gtk::Orientation::HORIZONTAL, 2};
    Gtk::Button         m_btn_sidebar, m_btn_open;
    Gtk::Button         m_btn_prev,    m_btn_next;
    Gtk::SpinButton     m_spin_page;
    Gtk::Label          m_lbl_total;
    Gtk::Button         m_btn_zout, m_btn_zin, m_btn_zfit;
    Gtk::Label          m_lbl_zoom;
    Gtk::Button         m_btn_rot_cw, m_btn_rot_ccw;
    Gtk::Button         m_btn_search, m_btn_fs;

    SearchBar           m_searchbar;
    bool                m_search_visible = false;

    Gtk::Paned          m_paned     {Gtk::Orientation::HORIZONTAL};
    bool                m_sidebar_visible = true;

    Gtk::Box            m_sidebar   {Gtk::Orientation::VERTICAL};
    Gtk::Label          m_sidebar_lbl;
    Gtk::ScrolledWindow m_tree_scroll;
    BookmarkColumns     m_bcols;
    Glib::RefPtr<Gtk::TreeStore> m_tree_store;
    Gtk::TreeView       m_tree_view;

    Gtk::ScrolledWindow m_scroll;
    MultiPageView       m_view;

    Gtk::Box            m_statusbar {Gtk::Orientation::HORIZONTAL};
    Gtk::Label          m_lbl_path;
    Gtk::Label          m_lbl_pagenum;

    bool  m_fullscreen = false;

    // ── Toolbar helpers ───────────────────────────────────
    void tb_btn(Gtk::Button& b, const char* icon, const char* tip, int min_w = 34) {
        b.set_label(icon); b.set_tooltip_text(tip);
        b.set_size_request(min_w, 34); m_toolbar.append(b);
    }
    void tb_sep() {
        auto* s = Gtk::make_managed<Gtk::Box>();
        s->add_css_class("sep"); s->set_size_request(1, 22);
        m_toolbar.append(*s);
    }
    void tb_spacer() {
        auto* sp = Gtk::make_managed<Gtk::Box>();
        sp->set_hexpand(true); m_toolbar.append(*sp);
    }

    // ── build_ui ──────────────────────────────────────────
    void build_ui() {
        m_toolbar.add_css_class("pdf-toolbar");
        tb_btn(m_btn_sidebar, "☰",  "Toggle bookmarks  B");
        tb_sep();
        tb_btn(m_btn_open,    "📂", "Open PDF  Ctrl+O", 38);
        tb_sep();
        tb_btn(m_btn_prev, "‹", "Previous page  ←");
        m_spin_page.set_range(1, 9999); m_spin_page.set_increments(1, 10);
        m_spin_page.set_value(1); m_spin_page.set_numeric(true);
        m_spin_page.add_css_class("page-spin");
        m_spin_page.set_tooltip_text("Jump to page");
        m_toolbar.append(m_spin_page);
        m_lbl_total.set_text("/ —"); m_lbl_total.add_css_class("page-label");
        m_lbl_total.set_margin_start(3); m_lbl_total.set_margin_end(3);
        m_toolbar.append(m_lbl_total);
        tb_btn(m_btn_next, "›", "Next page  →");
        tb_sep();
        tb_btn(m_btn_zout, "－", "Zoom out  −");
        m_lbl_zoom.set_text("100%"); m_lbl_zoom.add_css_class("zoom-label");
        m_lbl_zoom.set_xalign(0.5); m_toolbar.append(m_lbl_zoom);
        tb_btn(m_btn_zin,  "＋", "Zoom in  +");
        tb_btn(m_btn_zfit, "↔",  "Fit to width  W", 30);
        tb_sep();
        tb_btn(m_btn_rot_ccw, "↺", "Rotate left");
        tb_btn(m_btn_rot_cw,  "↻", "Rotate right");
        tb_sep();
        tb_spacer();
        tb_btn(m_btn_search, "🔍", "Find text  Ctrl+F");
        tb_btn(m_btn_fs,     "⛶",  "Full screen  F11");

        m_sidebar.add_css_class("pdf-sidebar"); m_sidebar.set_size_request(210, -1);
        m_sidebar_lbl.set_text("BOOKMARKS"); m_sidebar_lbl.set_xalign(0.0);
        m_sidebar_lbl.add_css_class("sidebar-title"); m_sidebar.append(m_sidebar_lbl);
        m_tree_store = Gtk::TreeStore::create(m_bcols);
        m_tree_view.set_model(m_tree_store); m_tree_view.set_headers_visible(false);
        m_tree_view.append_column("Title", m_bcols.title);
        m_tree_view.set_activate_on_single_click(true);
        m_tree_scroll.set_child(m_tree_view); m_tree_scroll.set_expand(true);
        m_tree_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
        m_sidebar.append(m_tree_scroll);

        m_scroll.set_expand(true);
        m_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
        m_scroll.set_child(m_view);

        m_paned.set_start_child(m_sidebar); m_paned.set_end_child(m_scroll);
        m_paned.set_position(210); m_paned.set_shrink_start_child(false);
        m_paned.set_resize_start_child(false); m_paned.set_expand(true);

        m_searchbar.set_visible(false);

        m_statusbar.add_css_class("pdf-statusbar");
        m_lbl_path.set_xalign(0.0); m_lbl_path.set_hexpand(true);
        m_lbl_path.set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        m_lbl_path.set_text("No document open"); m_lbl_pagenum.set_xalign(1.0);
        m_statusbar.append(m_lbl_path); m_statusbar.append(m_lbl_pagenum);

        m_root.append(m_toolbar); m_root.append(m_searchbar);
        m_root.append(m_paned);   m_root.append(m_statusbar);
        set_child(m_root);
        update_ui_state();
    }

    // ── connect_signals ───────────────────────────────────
    void connect_signals() {
        m_btn_open.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_open));
        m_btn_prev.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_prev));
        m_btn_next.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_next));
        m_btn_zin.signal_clicked().connect([this]{ m_view.zoom_in();    update_zoom_label(); });
        m_btn_zout.signal_clicked().connect([this]{ m_view.zoom_out();   update_zoom_label(); });
        m_btn_zfit.signal_clicked().connect([this]{ m_view.zoom_fit_w(); update_zoom_label(); });
        m_btn_rot_cw.signal_clicked().connect([this]{ m_view.rotate_cw();  update_zoom_label(); });
        m_btn_rot_ccw.signal_clicked().connect([this]{ m_view.rotate_ccw(); update_zoom_label(); });
        m_btn_sidebar.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::toggle_sidebar));
        m_btn_search.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::toggle_search));
        m_btn_fs.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::toggle_fullscreen));

        m_spin_page.signal_value_changed().connect([this]{
            int pg = (int)m_spin_page.get_value() - 1;
            if (pg != m_view.current_page()) m_view.scroll_to_page(pg);
        });
        m_view.signal_page_changed().connect(sigc::mem_fun(*this, &MainWindow::on_page_changed));
        m_view.signal_search_changed().connect(
            [this](int idx, int tot){ m_searchbar.set_count(idx, tot); });
        m_view.signal_request_scroll().connect(sigc::mem_fun(*this, &MainWindow::scroll_to_page));

        auto vadj = m_scroll.get_vadjustment();
        vadj->signal_value_changed().connect(sigc::mem_fun(*this, &MainWindow::on_scroll_changed));

        m_tree_view.signal_row_activated().connect(
            [this](const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn*) {
                auto it = m_tree_store->get_iter(path);
                if (it) scroll_to_page((*it)[m_bcols.page]);
            });
        m_searchbar.signal_search.connect([this](const std::string& text){
            int n = m_view.search_all(text);
            m_searchbar.set_count(m_view.hit_index(), n);
        });
        m_searchbar.signal_next.connect([this]{ m_view.search_next(); });
        m_searchbar.signal_prev.connect([this]{ m_view.search_prev(); });
        m_searchbar.signal_close.connect(sigc::mem_fun(*this, &MainWindow::close_search));

        auto key = Gtk::EventControllerKey::create();
        key->signal_key_pressed().connect(sigc::mem_fun(*this, &MainWindow::on_key), false);
        add_controller(key);
    }

    // ── Scroll ────────────────────────────────────────────
    void scroll_to_page(int idx) {
        if (!m_view.has_doc()) return;
        auto vadj = m_scroll.get_vadjustment();
        if (vadj) vadj->set_value((double)m_view.page_y_offset(idx));
        m_view.notify_visible_page(idx);
    }
    void on_scroll_changed() {
        if (!m_view.has_doc()) return;
        auto vadj = m_scroll.get_vadjustment();
        if (!vadj) return;
        double scroll_y = vadj->get_value();
        double view_h   = vadj->get_page_size();
        m_view.notify_scroll_position(scroll_y, view_h);
        double mid_y = scroll_y + view_h / 2.0;
        int best = 0; double best_dist = 1e9;
        int n = m_view.total_pages();
        for (int i = 0; i < n; ++i) {
            double dist = std::abs((double)m_view.page_y_offset(i) - mid_y);
            if (dist < best_dist) { best_dist = dist; best = i; }
        }
        m_view.notify_visible_page(best);
    }

    // ── Keyboard ──────────────────────────────────────────
    bool on_key(guint key, guint, Gdk::ModifierType mod) {
        bool ctrl = (mod & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK;
        if (ctrl && key == GDK_KEY_o) { on_open();           return true; }
        if (ctrl && key == GDK_KEY_f) { toggle_search();     return true; }
        if (key  == GDK_KEY_F11)      { toggle_fullscreen(); return true; }
        if (key  == GDK_KEY_Escape) {
            if (m_fullscreen)     { toggle_fullscreen(); return true; }
            if (m_search_visible) { close_search();      return true; }
        }
        if (key == GDK_KEY_b || key == GDK_KEY_B) { toggle_sidebar(); return true; }

        auto vadj = m_scroll.get_vadjustment();
        if (vadj) {
            double cur  = vadj->get_value();
            double lo   = vadj->get_lower();
            double hi   = vadj->get_upper() - vadj->get_page_size();
            double page = vadj->get_page_size();
            if (key == GDK_KEY_Down)
                { vadj->set_value(std::min(cur + SCROLL_STEP, hi)); return true; }
            if (key == GDK_KEY_Up)
                { vadj->set_value(std::max(cur - SCROLL_STEP, lo)); return true; }
            if (key == GDK_KEY_Page_Down || key == GDK_KEY_Right)
                { vadj->set_value(std::min(cur + page * 0.9, hi));  return true; }
            if (key == GDK_KEY_Page_Up || key == GDK_KEY_Left)
                { vadj->set_value(std::max(cur - page * 0.9, lo));  return true; }
            if (key == GDK_KEY_Home) { vadj->set_value(lo); return true; }
            if (key == GDK_KEY_End)  { vadj->set_value(hi); return true; }
        }

        if (key == GDK_KEY_plus || key == GDK_KEY_equal)
            { m_view.zoom_in();    update_zoom_label(); return true; }
        if (key == GDK_KEY_minus)
            { m_view.zoom_out();   update_zoom_label(); return true; }
        if (key == GDK_KEY_w || key == GDK_KEY_W)
            { m_view.zoom_fit_w(); update_zoom_label(); return true; }
        if (key == GDK_KEY_0)
            { m_view.zoom_100();   update_zoom_label(); return true; }
        if (key == GDK_KEY_r || key == GDK_KEY_R)
            { m_view.rotate_cw();  update_zoom_label(); return true; }
        return false;
    }

    // ── Actions ───────────────────────────────────────────
    void on_open() {
        auto dlg = Gtk::FileDialog::create();
        dlg->set_title("Open PDF");
        auto filter = Gtk::FileFilter::create();
        filter->set_name("PDF Files");
        filter->add_mime_type("application/pdf");
        filter->add_pattern("*.pdf");
        auto filters = Gio::ListStore<Gtk::FileFilter>::create();
        filters->append(filter); dlg->set_filters(filters);
        dlg->open(*this, [this, dlg](Glib::RefPtr<Gio::AsyncResult>& res){
            try {
                auto file = dlg->open_finish(res);
                if (!file) return;
                std::string path = file->get_path();
                if (m_view.load_document(path)) {
                    set_title(file->get_basename() + " — PDF Reader");
                    populate_outline(); update_ui_state();
                    m_lbl_path.set_text(path);
                    auto vadj = m_scroll.get_vadjustment();
                    if (vadj) vadj->set_value(0);
                } else { show_error("Cannot open file:\n" + path); }
            } catch (const Glib::Error&) {}
        });
    }
    void on_prev() { int p = m_view.current_page()-1; if(p>=0) scroll_to_page(p); }
    void on_next() { int p = m_view.current_page()+1; if(p<m_view.total_pages()) scroll_to_page(p); }
    void on_page_changed(int current, int total) {
        m_spin_page.set_range(1, total > 0 ? total : 1);
        { bool was = m_spin_page.get_sensitive();
          m_spin_page.set_sensitive(false); m_spin_page.set_value(current + 1);
          m_spin_page.set_sensitive(was); }
        m_lbl_total.set_text("/ " + itos(total));
        m_lbl_pagenum.set_text(itos(current+1) + " / " + itos(total));
        update_zoom_label(); update_ui_state();
    }
    void toggle_sidebar()    { m_sidebar_visible = !m_sidebar_visible; m_sidebar.set_visible(m_sidebar_visible); }
    void toggle_search()     { m_search_visible = !m_search_visible; m_searchbar.set_visible(m_search_visible);
                               if (m_search_visible) m_searchbar.focus(); else close_search(); }
    void close_search()      { m_search_visible = false; m_searchbar.set_visible(false);
                               m_searchbar.clear(); m_view.clear_search(); }
    void toggle_fullscreen() { m_fullscreen = !m_fullscreen;
                               if (m_fullscreen) fullscreen(); else unfullscreen();
                               m_btn_fs.set_label(m_fullscreen ? "⊠" : "⛶"); }

    void populate_outline() {
        m_tree_store->clear();
        if (!m_view.has_doc()) return;
        PopplerIndexIter* iter = poppler_index_iter_new(m_view.doc());
        if (!iter) { auto row = *m_tree_store->append();
                     row[m_bcols.title] = "(No bookmarks)"; row[m_bcols.page] = -1; return; }
        fill_outline(iter, Gtk::TreeModel::iterator());
        poppler_index_iter_free(iter); m_tree_view.expand_all();
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
                        if (nd) { page = nd->page_num - 1; poppler_dest_free(nd); }
                    } else { page = dest->page_num - 1; }
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

    void update_zoom_label() {
        if (!m_view.has_doc()) { m_lbl_zoom.set_text("—"); return; }
        m_lbl_zoom.set_text(itos((int)std::round(m_view.zoom() * 100.0)) + "%");
    }
    void update_ui_state() {
        bool has = m_view.has_doc();
        int  cur = m_view.current_page(), tot = m_view.total_pages();
        m_btn_prev.set_sensitive(has && cur > 0);
        m_btn_next.set_sensitive(has && cur < tot - 1);
        m_spin_page.set_sensitive(has);
        m_btn_zin.set_sensitive(has); m_btn_zout.set_sensitive(has);
        m_btn_zfit.set_sensitive(has);
        m_btn_rot_cw.set_sensitive(has); m_btn_rot_ccw.set_sensitive(has);
        m_btn_search.set_sensitive(has);
        if (!has) { m_lbl_total.set_text("/ —"); m_lbl_zoom.set_text("—"); m_lbl_pagenum.set_text(""); }
    }
    void show_error(const std::string& msg) { Gtk::AlertDialog::create(msg)->show(*this); }
};

// ============================================================
//  main
// ============================================================
int main(int argc, char* argv[]) {
    auto app = Gtk::Application::create("org.example.pdfreader");
    return app->make_window_and_run<MainWindow>(argc, argv);
}
