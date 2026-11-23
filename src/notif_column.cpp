#include "notif_column.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include "notif_feed.h"
#include "services.h"
#include "start_widgets.h"
#include "theme.h"
#include "tokens.h"

#ifndef HALO_COMPOSITION_INTEROP_DEFINED
#define HALO_COMPOSITION_INTEROP_DEFINED
struct ICompositionDrawingSurfaceInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE BeginDraw(RECT const* updateRect, REFIID iid, void** updateObject, POINT* offset) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndDraw() = 0;
    virtual HRESULT STDMETHODCALLTYPE Resize(SIZE size) = 0;
    virtual HRESULT STDMETHODCALLTYPE Scroll(RECT const* scrollRect, RECT const* clipRect, int dx, int dy) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResumeDraw() = 0;
    virtual HRESULT STDMETHODCALLTYPE SuspendDraw() = 0;
};
__CRT_UUID_DECL(ICompositionDrawingSurfaceInterop, 0xFD04E6E3, 0xFE0C, 0x4C3C, 0xAB, 0x19, 0xA0, 0x76, 0x01, 0xA5, 0x76, 0xEE)
#endif

namespace wuc = winrt::Windows::UI::Composition;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace halo {
namespace {

// Logical px; every size is a token (src/tokens.h) or a plain multiple of one.
constexpr float kMargin = ui::kS4, kGap = ui::kS3, kHeadH = 28, kCardPadX = ui::kS4, kCardPadY = ui::kS3, kIcon = ui::kIconTile;
constexpr float kTitleH = 20, kLineH = 20, kClose = ui::kS6, kScrollStep = 64, kClearW = 2 * ui::kButton, kEmptyH = 112;
constexpr int kMaxRows = 30, kMaxBodyLines = 2;
constexpr float kCalPad = ui::kS3, kCalNav = ui::kIconTile, kCalDow = ui::kS6, kCalRow = ui::kIconTile;  // the calendar card: padding, month row, weekday letters, a week
constexpr int kHoverMs = ui::kFastMs, kScrollSpringMs = 85;

winrt::Windows::UI::Color Col(D2D1_COLOR_F c) {
    auto b = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return winrt::Windows::UI::ColorHelper::FromArgb(b(c.a), b(c.r), b(c.g), b(c.b));
}
D2D1_COLOR_F Cf(theme::Argb a) { return {a.r / 255.0f, a.g / 255.0f, a.b / 255.0f, a.a / 255.0f}; }
bool In(D2D1_RECT_F const& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }

std::wstring Rel(long long ft) {
    FILETIME n;
    GetSystemTimeAsFileTime(&n);
    const long long now = (static_cast<long long>(n.dwHighDateTime) << 32) | n.dwLowDateTime;
    const long long sec = std::max<long long>(0, (now - ft) / 10000000);
    if (sec < 60) return L"now";
    if (sec < 3600) return std::to_wstring(sec / 60) + L"m";
    if (sec < 86400) return std::to_wstring(sec / 3600) + L"h";
    return std::to_wstring(sec / 86400) + L"d";
}

}  // namespace

struct NotifColumn::Impl {
    NotifColumn* api;
    HINSTANCE inst;
    wuc::Compositor comp;
    wuc::CompositionGraphicsDevice gdev;
    notifs::Feed feed;
    bool feedStarted = false, active = false, built = false;

    winrt::com_ptr<IDWriteFactory> dw;
    winrt::com_ptr<IDWriteTextFormat> fHead, fBtn, fApp, fTitle, fBody, fMonth, fDow, fDay, fDayB, fEmpty, fEmptyB, fGlyph;

    float W = 0, H = 0, s = 1;

    // visuals
    wuc::ContainerVisual root{nullptr}, listClip{nullptr}, listHold{nullptr};
    wuc::SpriteVisual headV{nullptr}, calV{nullptr}, listV{nullptr}, closeV{nullptr};
    wuc::CompositionDrawingSurface headS{nullptr}, calS{nullptr}, listS{nullptr}, closeS{nullptr};
    wuc::ShapeVisual plateV{nullptr};
    wuc::CompositionRoundedRectangleGeometry plateG{nullptr};
    wuc::CompositionColorBrush plateB{nullptr};
    wuc::CubicBezierEasingFunction ease{nullptr};
    float listSurfaceH = 0;

    // layout (column-local physical px)
    struct Row {
        notifs::Notif n;
        std::wstring body;  // cut to kMaxBodyLines
        float y = 0, h = 0;  // in the list's content space
    };
    std::vector<Row> rows;
    float contentH = 0, scroll = 0;
    float listTop = 0, listH = 0, calTop = 0, calH = 0, headTop = 0;
    int calRows = 6;  // week rows the shown month needs (4 to 6): the card is as tall as that, and the list below uses the rest
    D2D1_RECT_F clearR{}, prevR{}, nextR{}, monthR{};
    int calYear = 0, calMonth = 1, todayY = 0, todayM = 1, todayD = 1, firstDow = 0;

    enum class Kind { None, Row, Close, Clear, Prev, Next, Month };
    Kind hover = Kind::None, pressKind = Kind::None;
    int hoverRow = -1, pressRow = -1;
    bool down = false;  // a button is down: the control under it shows its pressed fill

    Impl(NotifColumn* a, HINSTANCE i, wuc::Compositor c, wuc::CompositionGraphicsDevice g)
        : api(a), inst(i), comp(std::move(c)), gdev(std::move(g)) {
        ease = comp.CreateCubicBezierEasingFunction({0.2f, 0.0f}, {0.0f, 1.0f});
    }

    // ---- helpers ----
    wuc::CompositionDrawingSurface Surface(float w, float h) {
        return gdev.CreateDrawingSurface({std::max(1.0f, std::ceil(w)), std::max(1.0f, std::ceil(h))},
                                         winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                         winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
    }
    wuc::SpriteVisual SurfaceVisual(wuc::CompositionDrawingSurface const& surf, float w, float h) {
        auto b = comp.CreateSurfaceBrush(surf);
        b.Stretch(wuc::CompositionStretch::None);
        auto v = comp.CreateSpriteVisual();
        v.Size({w, h});
        v.Brush(b);
        return v;
    }
    template <class F>
    void Paint(wuc::CompositionDrawingSurface const& surf, F&& draw) {
        if (!surf) return;
        auto interop = surf.as<ICompositionDrawingSurfaceInterop>();
        winrt::com_ptr<ID2D1DeviceContext> dc;
        POINT off{};
        if (FAILED(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off))) return;
        dc->Clear(D2D1::ColorF(0, 0.0f));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        try { draw(dc.get(), static_cast<float>(off.x), static_cast<float>(off.y)); } catch (...) {}
        interop->EndDraw();
    }
    winrt::com_ptr<IDWriteTextFormat> Format(float px, DWRITE_FONT_WEIGHT weight, bool wrap = false,
                                             DWRITE_PARAGRAPH_ALIGNMENT para = DWRITE_PARAGRAPH_ALIGNMENT_NEAR) {
        winrt::com_ptr<IDWriteTextFormat> f;
        dw->CreateTextFormat(ui::kFontText, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px * s, L"en-us", f.put());
        if (f) {
            f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
            f->SetParagraphAlignment(para);
        }
        return f;
    }
    winrt::com_ptr<IDWriteTextLayout> Layout(std::wstring const& t, IDWriteTextFormat* f, float w, float h, bool ellipsis = false) {
        winrt::com_ptr<IDWriteTextLayout> l;
        dw->CreateTextLayout(t.c_str(), static_cast<UINT32>(t.size()), f, std::max(w, 1.0f), std::max(h, 1.0f), l.put());
        if (l && ellipsis) {
            winrt::com_ptr<IDWriteInlineObject> sign;
            if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(f, sign.put()))) {
                DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                l->SetTrimming(&tr, sign.get());
            }
        }
        return l;
    }
    void Text(ID2D1DeviceContext* dc, std::wstring const& t, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool ellipsis = false) {
        auto l = Layout(t, f, r.right - r.left, r.bottom - r.top, ellipsis);
        if (!l) return;
        l->SetTextAlignment(align);
        winrt::com_ptr<ID2D1SolidColorBrush> b;
        dc->CreateSolidColorBrush(c, b.put());
        dc->DrawTextLayout(D2D1::Point2F(r.left, r.top), l.get(), b.get());
    }
    void Fill(ID2D1DeviceContext* dc, D2D1_RECT_F r, float radius, D2D1_COLOR_F c) {
        winrt::com_ptr<ID2D1SolidColorBrush> b;
        dc->CreateSolidColorBrush(c, b.put());
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b.get());
    }
    // A card: the resting fill and the contrast setting's hairline edge (1 device px: this file draws in device px).
    void Card(ID2D1DeviceContext* dc, D2D1_RECT_F r, float radius) {
        Fill(dc, r, radius, theme::P().fill);
        theme::TileEdge(dc, D2D1::RoundedRect(r, radius, radius), 1.0f);
    }
    void FillCorners(ID2D1DeviceContext* dc, D2D1_RECT_F r, float rt, float rb, D2D1_COLOR_F col) {
        winrt::com_ptr<ID2D1Factory> f;
        dc->GetFactory(f.put());
        winrt::com_ptr<ID2D1PathGeometry> g;
        winrt::com_ptr<ID2D1GeometrySink> sk;
        if (!f || FAILED(f->CreatePathGeometry(g.put())) || FAILED(g->Open(sk.put()))) { Fill(dc, r, std::max(rt, rb), col); return; }
        auto arc = [&](D2D1_POINT_2F to, float rad) { sk->AddArc(D2D1::ArcSegment(to, D2D1::SizeF(rad, rad), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL)); };
        sk->BeginFigure({r.left + rt, r.top}, D2D1_FIGURE_BEGIN_FILLED);
        sk->AddLine({r.right - rt, r.top});
        arc({r.right, r.top + rt}, rt);
        sk->AddLine({r.right, r.bottom - rb});
        arc({r.right - rb, r.bottom}, rb);
        sk->AddLine({r.left + rb, r.bottom});
        arc({r.left, r.bottom - rb}, rb);
        sk->AddLine({r.left, r.top + rt});
        arc({r.left + rt, r.top}, rt);
        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
        sk->Close();
        winrt::com_ptr<ID2D1SolidColorBrush> b;
        dc->CreateSolidColorBrush(col, b.put());
        dc->FillGeometry(g.get(), b.get());
    }
    void Animate(wuc::CompositionObject const& target, wchar_t const* prop, float to, int ms) {
        auto a = comp.CreateScalarKeyFrameAnimation();
        a.InsertKeyFrame(1.0f, to, ease);
        a.Duration(std::chrono::milliseconds(settings::Get().reduceMotion ? 1 : ms));
        target.StartAnimation(prop, a);
    }

    // ---- lifecycle ----
    wuc::ContainerVisual Build(float w, float h, float scale);
    void Release();
    void SetActive(bool on);
    void Relayout();
    void DrawHead();
    void DrawList();
    void DrawCal();
    void DrawClose();
    void SetToday();
    void ShiftMonth(int d);
    float CalH(int rows) const { return (kCalPad + kCalNav + kCalDow + rows * kCalRow + kCalPad) * s; }
    void Fit();     // positions below the calendar card for `calRows`
    void FitCal();  // the shown month changed: take its row count, move what is below, redo the list
    void Unpress();
    D2D1_COLOR_F HoverFill(Kind k) const { return down && pressKind == k ? Cf(theme::P().press) : theme::P().fillHover; }

    // ---- pointer ----
    struct Hit { Kind kind = Kind::None; int row = -1; };
    D2D1_RECT_F RowRect(int i) const { return R(kMargin * s, rows[i].y, W - kMargin * s, rows[i].y + rows[i].h); }  // content space
    D2D1_RECT_F CloseRect(int i) const {
        auto r = RowRect(i);
        return R(r.right - (kCardPadX - 2 + kClose) * s, r.top + (kCardPadY - 4) * s, r.right - (kCardPadX - 2) * s, r.top + (kCardPadY - 4 + kClose) * s);
    }
    Hit HitAt(float x, float y) const;
    void SetHover(Hit h);
    void ScrollTo(float v);
};

// ---------------------------------------------------------------------------------------------------------------
// Build

wuc::ContainerVisual NotifColumn::Impl::Build(float w, float h, float scale) {
    Release();
    W = w; H = h; s = scale;
    if (!dw) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.put()));
    fHead = Format(ui::kBody, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fBtn = Format(ui::kCaption, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fApp = Format(ui::kCaption, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fTitle = Format(ui::kBody, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fBody = Format(ui::kBody, DWRITE_FONT_WEIGHT_NORMAL, true);
    fMonth = Format(ui::kBody, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fDow = Format(ui::kCaption, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fDay = Format(ui::kCaption, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fDayB = Format(ui::kCaption, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fEmpty = Format(ui::kCaption, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fEmptyB = Format(ui::kBody, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fGlyph = Format(ui::kBody, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    wchar_t first[4] = L"";
    firstDow = 0;
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, first, 4)) firstDow = (_wtoi(first) + 1) % 7;  // 0 = Monday there, 0 = Sunday here
    SetToday();

    root = comp.CreateContainerVisual();
    root.Size({W, H});

    // Top to bottom: the calendar card (month and arrows, weekday initials, the weeks the month needs), then the heading
    // "Notifications" with Clear all, then the notification cards.
    calTop = kMargin * s;
    calRows = start::MakeMonthGrid(calYear, calMonth, 0, firstDow).rows;
    Fit();

    headV = nullptr;
    headS = Surface(W, kHeadH * s);
    headV = SurfaceVisual(headS, W, kHeadH * s);
    headV.Offset({0, headTop, 0});
    root.Children().InsertAtTop(headV);

    calS = Surface(W - 2 * kMargin * s, CalH(6));  // room for six weeks: a month that needs fewer draws a shorter card in it
    calV = SurfaceVisual(calS, W - 2 * kMargin * s, CalH(6));
    calV.Offset({kMargin * s, calTop, 0});
    root.Children().InsertAtTop(calV);

    listClip = comp.CreateContainerVisual();
    listClip.Size({W, listH});
    listClip.Offset({0, listTop, 0});
    listClip.Clip(comp.CreateInsetClip());
    listHold = comp.CreateContainerVisual();
    listHold.Size({W, listH});
    listClip.Children().InsertAtTop(listHold);
    root.Children().InsertAtTop(listClip);

    // Hover plate (one shape that follows the card under the pointer: the palette's hover fill, the card's own radius) and
    // the x that appears on that card.
    plateG = comp.CreateRoundedRectangleGeometry();
    plateG.CornerRadius({ui::kRadiusLarge * s, ui::kRadiusLarge * s});
    auto sprite = comp.CreateSpriteShape(plateG);
    plateB = comp.CreateColorBrush(Col(theme::P().fillHover));
    sprite.FillBrush(plateB);
    plateV = comp.CreateShapeVisual();
    plateV.Size({W, listH});
    plateV.Shapes().Append(sprite);
    plateV.Opacity(0.0f);
    closeS = Surface(kClose * s, kClose * s);
    closeV = SurfaceVisual(closeS, kClose * s, kClose * s);
    closeV.Opacity(0.0f);
    DrawClose();

    built = true;
    listV = nullptr;
    listS = nullptr;
    listSurfaceH = 0;
    hover = Kind::None;
    hoverRow = -1;
    down = false;
    pressKind = Kind::None;
    scroll = 0;
    DrawHead();
    DrawCal();
    Relayout();
    return root;
}

void NotifColumn::Impl::Release() {
    built = false;
    root = nullptr; listClip = nullptr; listHold = nullptr;
    headV = nullptr; calV = nullptr; listV = nullptr; closeV = nullptr; plateV = nullptr; plateG = nullptr;
    headS = nullptr; calS = nullptr; listS = nullptr; closeS = nullptr; plateB = nullptr;
    rows.clear();
    fHead = fBtn = fApp = fTitle = fBody = fMonth = fDow = fDay = fDayB = fEmpty = fEmptyB = fGlyph = nullptr;
}

void NotifColumn::Impl::SetActive(bool on) {
    if (on == active) return;
    active = on;
    if (on) {
        if (!feedStarted) {
            feedStarted = true;
            feed.Start(inst, [this] { if (built && active) { Relayout(); DrawHead(); } });
        }
        if (built) { SetToday(); FitCal(); DrawCal(); }
        feed.SetActive(true);
    } else if (feedStarted) {
        feed.SetActive(false);
    }
}

void NotifColumn::Impl::SetToday() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    todayY = t.wYear; todayM = t.wMonth; todayD = t.wDay;
    calYear = todayY; calMonth = todayM;
}

void NotifColumn::Impl::ShiftMonth(int d) {
    calMonth += d;
    if (calMonth < 1) { calMonth = 12; calYear--; }
    if (calMonth > 12) { calMonth = 1; calYear++; }
    FitCal();
    DrawCal();
}

void NotifColumn::Impl::Fit() {
    calH = CalH(calRows);
    headTop = calTop + calH + kGap * s;
    listTop = headTop + kHeadH * s + ui::kS2 * s;
    listH = std::max(0.0f, H - kMargin * s - listTop);
}

void NotifColumn::Impl::FitCal() {
    if (!built) return;
    const int rows = start::MakeMonthGrid(calYear, calMonth, 0, firstDow).rows;
    if (rows == calRows) return;
    calRows = rows;
    Fit();
    headV.Offset({0, headTop, 0});
    listClip.Size({W, listH});
    listClip.Offset({0, listTop, 0});
    Relayout();  // the list's room changed: clamp the scroll, redraw the cards and the heading
}

// ---------------------------------------------------------------------------------------------------------------
// Drawing

void NotifColumn::Impl::DrawHead() {
    if (!built) return;
    auto const& pal = theme::P();
    const bool any = !rows.empty();
    Paint(headS, [&](ID2D1DeviceContext* dc, float ox, float oy) {
        const float h = kHeadH * s, cw = kClearW * s;
        Text(dc, L"Notifications", fHead.get(), R(ox + (kMargin + ui::kS1) * s, oy, ox + W - kMargin * s - cw, oy + h), pal.text);
        clearR = R(W - kMargin * s - cw, headTop, W - kMargin * s, headTop + h);  // column coordinates (the surface sits at headTop)
        if (any) {
            if (hover == Kind::Clear || (down && pressKind == Kind::Clear))
                Fill(dc, R(ox + clearR.left, oy, ox + clearR.right, oy + h), ui::kRadiusHover * s, HoverFill(Kind::Clear));
            Text(dc, L"Clear all", fBtn.get(), R(ox + clearR.left, oy, ox + clearR.right, oy + h), pal.textSoft, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    });
    if (!any) clearR = {};
}

void NotifColumn::Impl::DrawClose() {
    auto const& pal = theme::P();
    Paint(closeS, [&](ID2D1DeviceContext* dc, float ox, float oy) {
        const float d = kClose * s;
        winrt::com_ptr<ID2D1SolidColorBrush> b;
        dc->CreateSolidColorBrush(pal.fillPress, b.put());
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ox + d / 2, oy + d / 2), d / 2, d / 2), b.get());
        dc->CreateSolidColorBrush(pal.text, b.put());
        const float a = d * 0.28f, cx = ox + d / 2, cy = oy + d / 2, t = std::max(1.0f, ui::kHair * 1.5f * s);
        dc->DrawLine(D2D1::Point2F(cx - a, cy - a), D2D1::Point2F(cx + a, cy + a), b.get(), t);
        dc->DrawLine(D2D1::Point2F(cx - a, cy + a), D2D1::Point2F(cx + a, cy - a), b.get(), t);
    });
}

void NotifColumn::Impl::Relayout() {
    if (!built) return;
    auto const& pal = theme::P();
    (void)pal;
    auto items = feed.Items();
    if (items.size() > kMaxRows) items.resize(kMaxRows);
    const float cardW = W - 2 * kMargin * s, textX = (kCardPadX + kIcon + ui::kS3) * s, textW = cardW - textX - kCardPadX * s;
    rows.clear();
    float y = ui::kS1 * s;
    for (auto& n : items) {
        Row r;
        r.n = n;
        r.body = n.body;
        std::replace(r.body.begin(), r.body.end(), L'\n', L' ');
        int lines = 0;
        if (!r.body.empty()) {
            auto l = Layout(r.body, fBody.get(), textW, 1000);
            UINT32 count = 0;
            if (l) {
                l->GetLineMetrics(nullptr, 0, &count);
                std::vector<DWRITE_LINE_METRICS> lm(count);
                l->GetLineMetrics(lm.data(), count, &count);
                lines = static_cast<int>(std::min<UINT32>(count, kMaxBodyLines));
                if (count > static_cast<UINT32>(kMaxBodyLines)) {  // cut after the second line and end it with an ellipsis
                    UINT32 keep = 0;
                    for (int i = 0; i < kMaxBodyLines; i++) keep += lm[i].length;
                    r.body.resize(keep);
                    while (!r.body.empty() && (r.body.back() == L' ' || r.body.back() == L'\n' || r.body.back() == L'\r')) r.body.pop_back();
                    if (r.body.size() > 1) r.body.resize(r.body.size() - 1);
                    r.body += L"…";
                }
            }
        }
        r.h = std::round((kCardPadY + kTitleH + (lines ? ui::kS1 + lines * kLineH : 0) + kCardPadY) * s);  // title (with the age), then up to two lines
        r.y = y;
        y += r.h + kGap * s;  // separate cards, 12 px apart
        rows.push_back(std::move(r));
    }
    contentH = rows.empty() ? 0 : y;
    const float maxScroll = std::max(0.0f, contentH - listH);
    scroll = std::clamp(scroll, 0.0f, maxScroll);
    DrawList();
    DrawHead();
    SetHover({});  // a stale hover row would point past the new list
}

void NotifColumn::Impl::DrawList() {
    if (!built) return;
    auto const& pal = theme::P();
    const float h = std::max(contentH, listH);
    if (!listS || std::abs(h - listSurfaceH) > 0.5f) {  // a new size: a new surface (the old one is dropped with its visual)
        if (listV) listHold.Children().Remove(listV);
        listSurfaceH = h;
        listS = Surface(W, h);
        listV = SurfaceVisual(listS, W, h);
        listHold.Children().RemoveAll();
        listHold.Children().InsertAtTop(listV);
        listHold.Children().InsertAtTop(plateV);
        listHold.Children().InsertAtTop(closeV);
        listHold.Size({W, h});
        plateV.Size({W, h});
    }
    listHold.Offset({0, -scroll, 0});
    const float cardW = W - 2 * kMargin * s, textX = (kCardPadX + kIcon + ui::kS3) * s, textW = cardW - textX - kCardPadX * s;
    Paint(listS, [&](ID2D1DeviceContext* dc, float ox, float oy) {
        if (rows.empty()) {  // an empty state: one card, a bell, what it means
            const float l = ox + kMargin * s, t = oy + ui::kS1 * s, ch = kEmptyH * s;
            Card(dc, R(l, t, l + cardW, t + ch), ui::kRadiusLarge * s);
            const bool ready = feed.Ready();
            const float cx = l + cardW / 2, top = t + ui::kS4 * s;
            winrt::com_ptr<IDWriteTextFormat> fBell;
            dw->CreateTextFormat(ui::kFontIcons, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, ui::kIconTile * s, L"en-us", fBell.put());
            if (fBell) fBell->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            Text(dc, L"\uE7ED", fBell.get(), R(cx - ui::kS6 * s, top, cx + ui::kS6 * s, top + ui::kIconTile * s), theme::Muted(), DWRITE_TEXT_ALIGNMENT_CENTER);
            Text(dc, ready ? L"No new notifications" : L"Checking…", fEmptyB.get(), R(l, top + (ui::kIconTile + ui::kS2) * s, l + cardW, top + (ui::kIconTile + ui::kS2 + kTitleH) * s), pal.text, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (ready) Text(dc, L"You're all caught up.", fEmpty.get(), R(l, top + (ui::kIconTile + ui::kS2 + kTitleH) * s, l + cardW, top + (ui::kIconTile + ui::kS2 + kTitleH + ui::kS4) * s), theme::Muted(), DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        for (size_t ri = 0; ri < rows.size(); ri++) {
            auto const& r = rows[ri];
            const float l = ox + kMargin * s, t = oy + r.y;
            Card(dc, R(l, t, l + cardW, t + r.h), ui::kRadiusLarge * s);  // a separate radius-20 card
            // icon: the app's own, else a letter tile
            const float ix = l + kCardPadX * s, iy = t + kCardPadY * s, isz = kIcon * s;
            if (auto const* ic = feed.Icon(r.n.appKey); ic && ic->w && ic->h) {
                winrt::com_ptr<ID2D1Bitmap> bmp;
                if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(ic->w, ic->h), ic->px.data(), ic->w * 4,
                                               D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), bmp.put())))
                    dc->DrawBitmap(bmp.get(), R(ix, iy, ix + isz, iy + isz), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                Fill(dc, R(ix, iy, ix + isz, iy + isz), ui::kS1 * 1.5f * s, pal.accent);
                std::wstring letter = r.n.app.empty() ? L"?" : std::wstring(1, static_cast<wchar_t>(std::towupper(r.n.app[0])));
                Text(dc, letter, fTitle.get(), R(ix, iy, ix + isz, iy + isz), pal.onAccent, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            const float tx = l + textX, ty = t + kCardPadY * s;
            const std::wstring age = Rel(r.n.ft);  // 14 semibold title, the age as a caption at the right end of the line
            const float ageW = 40 * s;
            Text(dc, r.n.title.empty() ? r.n.app : r.n.title, fTitle.get(), R(tx, ty, tx + textW - ageW, ty + kTitleH * s), pal.text, DWRITE_TEXT_ALIGNMENT_LEADING, true);
            Text(dc, age, fApp.get(), R(tx + textW - ageW, ty, tx + textW, ty + kTitleH * s), theme::Muted(), DWRITE_TEXT_ALIGNMENT_TRAILING);
            if (!r.body.empty())
                Text(dc, r.body, fBody.get(), R(tx, ty + (kTitleH + ui::kS1) * s, tx + textW, t + r.h - kCardPadY * s), pal.textSoft);
        }
    });
}

void NotifColumn::Impl::DrawCal() {
    if (!built) return;
    auto const& pal = theme::P();
    const float w = W - 2 * kMargin * s;
    const bool thisMonth = calYear == todayY && calMonth == todayM;
    const auto grid = start::MakeMonthGrid(calYear, calMonth, thisMonth ? todayD : 0, firstDow);
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(calYear); st.wMonth = static_cast<WORD>(calMonth); st.wDay = 1;
    wchar_t title[64] = L"";
    if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"MMMM yyyy", title, 64, nullptr)) wcscpy(title, L"Calendar");

    const float pad = kCalPad * s, navTop = pad, navH = kCalNav * s, dowTop = navTop + navH, dowH = kCalDow * s, rowsTop = dowTop + dowH, rowH = kCalRow * s;
    const float btn = kCalNav * s;
    // hit rects, column coordinates (the surface sits at x = kMargin * s, y = calTop)
    const float x0 = kMargin * s;
    monthR = R(x0 + pad, calTop + navTop, x0 + w - pad - 2 * btn, calTop + navTop + navH);
    prevR = R(x0 + w - pad - 2 * btn, calTop + navTop, x0 + w - pad - btn, calTop + navTop + navH);
    nextR = R(x0 + w - pad - btn, calTop + navTop, x0 + w - pad, calTop + navTop + navH);

    Paint(calS, [&](ID2D1DeviceContext* dc, float ox, float oy) {
        Card(dc, R(ox, oy, ox + w, oy + calH), ui::kRadiusLarge * s);  // the card
        winrt::com_ptr<ID2D1SolidColorBrush> b;
        Text(dc, title, fMonth.get(), R(ox + pad, oy + navTop, ox + w - pad - 2 * btn, oy + navTop + navH), pal.text);
        for (int i = 0; i < 2; i++) {
            const Kind k = i == 0 ? Kind::Prev : Kind::Next;
            const float bx = ox + w - pad - (2 - i) * btn;
            if (hover == k || (down && pressKind == k))
                Fill(dc, R(bx, oy + navTop, bx + btn, oy + navTop + navH), ui::kRadiusHover * s, HoverFill(k));
            Text(dc, i == 0 ? L"‹" : L"›", fGlyph.get(), R(bx, oy + navTop, bx + btn, oy + navTop + navH), pal.textSoft, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        static const wchar_t* kDow[7] = {L"S", L"M", L"T", L"W", L"T", L"F", L"S"};
        const float cw = (w - 2 * pad) / 7, gx = ox + pad;
        for (int c = 0; c < 7; c++)
            Text(dc, kDow[(firstDow + c) % 7], fDow.get(), R(gx + c * cw, oy + dowTop, gx + (c + 1) * cw, oy + dowTop + dowH), theme::Muted(), DWRITE_TEXT_ALIGNMENT_CENTER);
        for (int i = 0; i < grid.rows * 7; i++) {
            if (!grid.day[i]) continue;
            const int r = i / 7, c = i % 7;
            const D2D1_RECT_F cell = R(gx + c * cw, oy + rowsTop + r * rowH, gx + (c + 1) * cw, oy + rowsTop + (r + 1) * rowH);
            const bool today = i == grid.todayCell;
            if (today) {  // today: a solid accent circle with white ink
                const float d = rowH, cx = (cell.left + cell.right) / 2, cy = (cell.top + cell.bottom) / 2;
                dc->CreateSolidColorBrush(pal.accent, b.put());
                dc->FillEllipse(D2D1::Ellipse({cx, cy}, d / 2, d / 2), b.get());
            }
            const int dow = (firstDow + c) % 7;
            Text(dc, std::to_wstring(grid.day[i]), (today ? fDayB : fDay).get(), cell, today ? pal.onAccent : (dow == 0 || dow == 6 ? theme::Muted() : pal.textSoft), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    });
}

// ---------------------------------------------------------------------------------------------------------------
// Pointer

NotifColumn::Impl::Hit NotifColumn::Impl::HitAt(float x, float y) const {
    if (!built) return {};
    if (In(clearR, x, y)) return {Kind::Clear, -1};
    if (In(prevR, x, y)) return {Kind::Prev, -1};
    if (In(nextR, x, y)) return {Kind::Next, -1};
    if (In(monthR, x, y)) return {Kind::Month, -1};
    if (y >= listTop && y < listTop + listH && x >= kMargin * s && x < W - kMargin * s) {
        const float cy = y - listTop + scroll;
        for (int i = 0; i < static_cast<int>(rows.size()); i++)
            if (cy >= rows[i].y && cy < rows[i].y + rows[i].h) {
                const auto c = CloseRect(i);
                return {In(c, x, cy) ? Kind::Close : Kind::Row, i};
            }
    }
    return {};
}

void NotifColumn::Impl::SetHover(Hit h) {
    if (!built || (h.kind == hover && h.row == hoverRow)) return;
    const Kind old = hover;
    hover = h.kind;
    hoverRow = h.row;
    if (old == Kind::Clear || hover == Kind::Clear) DrawHead();
    if (old == Kind::Prev || old == Kind::Next || hover == Kind::Prev || hover == Kind::Next) DrawCal();
    const bool onCard = (hover == Kind::Row || hover == Kind::Close) && hoverRow >= 0 && hoverRow < static_cast<int>(rows.size());
    if (onCard) {
        auto r = RowRect(hoverRow);
        plateG.StopAnimation(L"Size");  // lands on the card, no glide (a gliding plate dragged on every scroll step)
        plateG.StopAnimation(L"Offset");
        plateG.Size({r.right - r.left, r.bottom - r.top});
        plateG.Offset({r.left, r.top});
        Animate(plateV, L"Opacity", 1.0f, kHoverMs);
        auto c = CloseRect(hoverRow);
        closeV.Offset({c.left, c.top, 0});
        Animate(closeV, L"Opacity", 1.0f, kHoverMs);
    } else {
        Animate(plateV, L"Opacity", 0.0f, kHoverMs);
        Animate(closeV, L"Opacity", 0.0f, kHoverMs);
    }
}

// Mouse-down: the control under the pointer takes the palette's pressed fill at once; Unpress (mouse-up) puts it back.
void NotifColumn::Impl::Unpress() {
    if (!built || !down) return;
    const Kind k = pressKind;
    down = false;
    pressKind = Kind::None;
    if (plateB) plateB.Color(Col(theme::P().fillHover));
    if (k == Kind::Clear) DrawHead();
    if (k == Kind::Prev || k == Kind::Next) DrawCal();
}

void NotifColumn::Impl::ScrollTo(float v) {
    const float maxScroll = std::max(0.0f, contentH - listH);
    scroll = std::clamp(v, 0.0f, maxScroll);
    if (settings::Get().reduceMotion) {
        listHold.StopAnimation(L"Offset.Y");
        listHold.Offset({0, -scroll, 0});
        return;
    }
    // Critically damped spring (an exponential ease-out): a new wheel notch moves the target, velocity carries over.
    auto a = comp.CreateSpringScalarAnimation();
    a.FinalValue(-scroll);
    a.DampingRatio(1.0f);
    a.Period(std::chrono::milliseconds(kScrollSpringMs));
    listHold.StartAnimation(L"Offset.Y", a);
}

// ---------------------------------------------------------------------------------------------------------------

NotifColumn::NotifColumn(HINSTANCE inst, wuc::Compositor comp, wuc::CompositionGraphicsDevice gdev)
    : m(std::make_unique<Impl>(this, inst, std::move(comp), std::move(gdev))) {}
NotifColumn::~NotifColumn() { m->feed.Stop(); }

wuc::ContainerVisual NotifColumn::Build(float w, float h, float scale) { return m->Build(w, h, scale); }
void NotifColumn::Release() { m->Release(); }
void NotifColumn::SetActive(bool on) { m->SetActive(on); }
bool NotifColumn::Contains(float x, float y) const { return m->built && x >= 0 && y >= 0 && x < m->W && y < m->H; }

void NotifColumn::Move(float x, float y) { m->SetHover(m->HitAt(x, y)); }
void NotifColumn::Leave() { m->SetHover({}); }

void NotifColumn::Press(float x, float y) {
    if (!m->built || !Contains(x, y)) return;
    const auto h = m->HitAt(x, y);
    if (h.kind == Impl::Kind::None) return;
    m->down = true;
    m->pressKind = h.kind;
    m->pressRow = h.row;
    switch (h.kind) {
        case Impl::Kind::Clear: m->DrawHead(); break;
        case Impl::Kind::Prev:
        case Impl::Kind::Next: m->DrawCal(); break;
        case Impl::Kind::Row:
        case Impl::Kind::Close:
            if (h.row >= 0 && h.row < static_cast<int>(m->rows.size())) {  // the card's plate goes to the pressed fill, now (even before any hover)
                auto r = m->RowRect(h.row);
                m->plateG.StopAnimation(L"Size");
                m->plateG.StopAnimation(L"Offset");
                m->plateG.Size({r.right - r.left, r.bottom - r.top});
                m->plateG.Offset({r.left, r.top});
                m->plateV.StopAnimation(L"Opacity");
                m->plateV.Opacity(1.0f);
                m->plateB.Color(Col(theme::P().press.a ? Cf(theme::P().press) : theme::P().fillHover));
            }
            break;
        default: break;
    }
}

void NotifColumn::Unpress() { m->Unpress(); }

bool NotifColumn::Wheel(float x, float y, int delta) {
    if (!Contains(x, y) || m->contentH <= m->listH) return Contains(x, y);
    m->ScrollTo(m->scroll - static_cast<float>(delta) / WHEEL_DELTA * kScrollStep * m->s);
    m->SetHover(m->HitAt(x, y));
    return true;
}

bool NotifColumn::Click(float x, float y) {
    if (!Contains(x, y)) return false;
    const auto h = m->HitAt(x, y);
    switch (h.kind) {
        case Impl::Kind::Clear: m->feed.Clear(); break;
        case Impl::Kind::Prev: m->ShiftMonth(-1); break;
        case Impl::Kind::Next: m->ShiftMonth(1); break;
        case Impl::Kind::Month: m->SetToday(); m->FitCal(); m->DrawCal(); break;
        case Impl::Kind::Close:
            m->feed.Remove({m->rows[h.row].n.id});
            break;
        case Impl::Kind::Row: {
            const auto n = m->rows[h.row].n;
            if (m->feed.Open(n) && onOpened) onOpened();
            break;
        }
        default: break;
    }
    return true;
}

}  // namespace halo
