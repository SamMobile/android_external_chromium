// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/views/status_bubble_views.h"

#include <algorithm>

#include "app/linear_animation.h"
#include "app/l10n_util.h"
#include "app/resource_bundle.h"
#include "app/text_elider.h"
#include "base/i18n/rtl.h"
#include "base/message_loop.h"
#include "base/string_util.h"
#include "chrome/browser/browser_theme_provider.h"
#include "gfx/canvas_skia.h"
#include "gfx/point.h"
#include "googleurl/src/gurl.h"
#include "grit/generated_resources.h"
#include "grit/theme_resources.h"
#include "net/base/net_util.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkRect.h"
#include "views/controls/label.h"
#include "views/controls/scrollbar/native_scroll_bar.h"
#include "views/screen.h"
#include "views/widget/root_view.h"
#include "views/widget/widget.h"
#include "views/window/window.h"

using views::Widget;

// The alpha and color of the bubble's shadow.
static const SkColor kShadowColor = SkColorSetARGB(30, 0, 0, 0);

// The roundedness of the edges of our bubble.
static const int kBubbleCornerRadius = 4;

// How close the mouse can get to the infobubble before it starts sliding
// off-screen.
static const int kMousePadding = 20;

// The horizontal offset of the text within the status bubble, not including the
// outer shadow ring.
static const int kTextPositionX = 3;

// The minimum horizontal space between the (right) end of the text and the edge
// of the status bubble, not including the outer shadow ring.
static const int kTextHorizPadding = 1;

// Delays before we start hiding or showing the bubble after we receive a
// show or hide request.
static const int kShowDelay = 80;
static const int kHideDelay = 250;

// How long each fade should last for.
static const int kShowFadeDurationMS = 120;
static const int kHideFadeDurationMS = 200;
static const int kFramerate = 25;

// How long each expansion step should take.
static const int kMinExpansionStepDurationMS = 20;
static const int kMaxExpansionStepDurationMS = 150;

// View -----------------------------------------------------------------------
// StatusView manages the display of the bubble, applying text changes and
// fading in or out the bubble as required.
class StatusBubbleViews::StatusView : public views::Label,
                                      public LinearAnimation,
                                      public AnimationDelegate {
 public:
  StatusView(StatusBubble* status_bubble, views::Widget* popup,
             ThemeProvider* theme_provider)
      : ALLOW_THIS_IN_INITIALIZER_LIST(LinearAnimation(kFramerate, this)),
        stage_(BUBBLE_HIDDEN),
        style_(STYLE_STANDARD),
        ALLOW_THIS_IN_INITIALIZER_LIST(timer_factory_(this)),
        status_bubble_(status_bubble),
        popup_(popup),
        opacity_start_(0),
        opacity_end_(0),
        theme_provider_(theme_provider) {
    ResourceBundle& rb = ResourceBundle::GetSharedInstance();
    gfx::Font font(rb.GetFont(ResourceBundle::BaseFont));
    SetFont(font);
  }

  virtual ~StatusView() {
    Stop();
    CancelTimer();
  }

  // The bubble can be in one of many stages:
  enum BubbleStage {
    BUBBLE_HIDDEN,         // Entirely BUBBLE_HIDDEN.
    BUBBLE_HIDING_FADE,    // In a fade-out transition.
    BUBBLE_HIDING_TIMER,   // Waiting before a fade-out.
    BUBBLE_SHOWING_TIMER,  // Waiting before a fade-in.
    BUBBLE_SHOWING_FADE,   // In a fade-in transition.
    BUBBLE_SHOWN           // Fully visible.
  };

  enum BubbleStyle {
    STYLE_BOTTOM,
    STYLE_FLOATING,
    STYLE_STANDARD,
    STYLE_STANDARD_RIGHT
  };

  // Set the bubble text to a certain value, hides the bubble if text is
  // an empty string.  Trigger animation sequence to display if
  // |should_animate_open|.
  void SetText(const std::wstring& text, bool should_animate_open);

  BubbleStage GetState() const { return stage_; }

  void SetStyle(BubbleStyle style);

  BubbleStyle GetStyle() const { return style_; }

  // Show the bubble instantly.
  void Show();

  // Hide the bubble instantly.
  void Hide();

  // Resets any timers we have. Typically called when the user moves a
  // mouse.
  void ResetTimer();

 private:
  class InitialTimer;

  // Manage the timers that control the delay before a fade begins or ends.
  void StartTimer(int time);
  void OnTimer();
  void CancelTimer();
  void RestartTimer(int delay);

  // Manage the fades and starting and stopping the animations correctly.
  void StartFade(double start, double end, int duration);
  void StartHiding();
  void StartShowing();

  // Animation functions.
  double GetCurrentOpacity();
  void SetOpacity(double opacity);
  void AnimateToState(double state);
  void AnimationEnded(const Animation* animation);

  virtual void Paint(gfx::Canvas* canvas);

  BubbleStage stage_;
  BubbleStyle style_;

  ScopedRunnableMethodFactory<StatusBubbleViews::StatusView> timer_factory_;

  // Manager, owns us.
  StatusBubble* status_bubble_;

  // Handle to the widget that contains us.
  views::Widget* popup_;

  // The currently-displayed text.
  std::wstring text_;

  // Start and end opacities for the current transition - note that as a
  // fade-in can easily turn into a fade out, opacity_start_ is sometimes
  // a value between 0 and 1.
  double opacity_start_;
  double opacity_end_;

  // Holds the theme provider of the frame that created us.
  ThemeProvider* theme_provider_;
};

void StatusBubbleViews::StatusView::SetText(
    const std::wstring& text, bool should_animate_open) {
  if (text.empty()) {
    // The string was empty.
    StartHiding();
  } else {
    // We want to show the string.
    text_ = text;
    if (should_animate_open)
      StartShowing();
  }

  SchedulePaint();
}

void StatusBubbleViews::StatusView::Show() {
  Stop();
  CancelTimer();
  SetOpacity(1.0);
  popup_->Show();
  stage_ = BUBBLE_SHOWN;
  PaintNow();
}

void StatusBubbleViews::StatusView::Hide() {
  Stop();
  CancelTimer();
  SetOpacity(0.0);
  text_.clear();
  popup_->Hide();
  stage_ = BUBBLE_HIDDEN;
}

void StatusBubbleViews::StatusView::StartTimer(int time) {
  if (!timer_factory_.empty())
    timer_factory_.RevokeAll();

  MessageLoop::current()->PostDelayedTask(FROM_HERE,
      timer_factory_.NewRunnableMethod(&StatusBubbleViews::StatusView::OnTimer),
      time);
}

void StatusBubbleViews::StatusView::OnTimer() {
  if (stage_ == BUBBLE_HIDING_TIMER) {
    stage_ = BUBBLE_HIDING_FADE;
    StartFade(1.0, 0.0, kHideFadeDurationMS);
  } else if (stage_ == BUBBLE_SHOWING_TIMER) {
    stage_ = BUBBLE_SHOWING_FADE;
    StartFade(0.0, 1.0, kShowFadeDurationMS);
  }
}

void StatusBubbleViews::StatusView::CancelTimer() {
  if (!timer_factory_.empty())
    timer_factory_.RevokeAll();
}

void StatusBubbleViews::StatusView::RestartTimer(int delay) {
  CancelTimer();
  StartTimer(delay);
}

void StatusBubbleViews::StatusView::ResetTimer() {
  if (stage_ == BUBBLE_SHOWING_TIMER) {
    // We hadn't yet begun showing anything when we received a new request
    // for something to show, so we start from scratch.
    RestartTimer(kShowDelay);
  }
}

void StatusBubbleViews::StatusView::StartFade(double start,
                                              double end,
                                              int duration) {
  opacity_start_ = start;
  opacity_end_ = end;

  // This will also reset the currently-occurring animation.
  SetDuration(duration);
  Start();
}

void StatusBubbleViews::StatusView::StartHiding() {
  if (stage_ == BUBBLE_SHOWN) {
    stage_ = BUBBLE_HIDING_TIMER;
    StartTimer(kHideDelay);
  } else if (stage_ == BUBBLE_SHOWING_TIMER) {
    stage_ = BUBBLE_HIDDEN;
    CancelTimer();
  } else if (stage_ == BUBBLE_SHOWING_FADE) {
    stage_ = BUBBLE_HIDING_FADE;
    // Figure out where we are in the current fade.
    double current_opacity = GetCurrentOpacity();

    // Start a fade in the opposite direction.
    StartFade(current_opacity, 0.0,
              static_cast<int>(kHideFadeDurationMS * current_opacity));
  }
}

void StatusBubbleViews::StatusView::StartShowing() {
  if (stage_ == BUBBLE_HIDDEN) {
    popup_->Show();
    stage_ = BUBBLE_SHOWING_TIMER;
    StartTimer(kShowDelay);
  } else if (stage_ == BUBBLE_HIDING_TIMER) {
    stage_ = BUBBLE_SHOWN;
    CancelTimer();
  } else if (stage_ == BUBBLE_HIDING_FADE) {
    // We're partway through a fade.
    stage_ = BUBBLE_SHOWING_FADE;

    // Figure out where we are in the current fade.
    double current_opacity = GetCurrentOpacity();

    // Start a fade in the opposite direction.
    StartFade(current_opacity, 1.0,
              static_cast<int>(kShowFadeDurationMS * current_opacity));
  } else if (stage_ == BUBBLE_SHOWING_TIMER) {
    // We hadn't yet begun showing anything when we received a new request
    // for something to show, so we start from scratch.
    ResetTimer();
  }
}

// Animation functions.
double StatusBubbleViews::StatusView::GetCurrentOpacity() {
  return opacity_start_ + (opacity_end_ - opacity_start_) *
         LinearAnimation::GetCurrentValue();
}

void StatusBubbleViews::StatusView::SetOpacity(double opacity) {
  popup_->SetOpacity(static_cast<unsigned char>(opacity * 255));
  SchedulePaint();
}

void StatusBubbleViews::StatusView::AnimateToState(double state) {
  SetOpacity(GetCurrentOpacity());
}

void StatusBubbleViews::StatusView::AnimationEnded(
    const Animation* animation) {
  SetOpacity(opacity_end_);

  if (stage_ == BUBBLE_HIDING_FADE) {
    stage_ = BUBBLE_HIDDEN;
    popup_->Hide();
  } else if (stage_ == BUBBLE_SHOWING_FADE) {
    stage_ = BUBBLE_SHOWN;
  }
}

void StatusBubbleViews::StatusView::SetStyle(BubbleStyle style) {
  if (style_ != style) {
    style_ = style;
    SchedulePaint();
  }
}

void StatusBubbleViews::StatusView::Paint(gfx::Canvas* canvas) {
  SkPaint paint;
  paint.setStyle(SkPaint::kFill_Style);
  paint.setFlags(SkPaint::kAntiAlias_Flag);
  SkColor toolbar_color =
      theme_provider_->GetColor(BrowserThemeProvider::COLOR_TOOLBAR);
  paint.setColor(toolbar_color);

  gfx::Rect popup_bounds;
  popup_->GetBounds(&popup_bounds, true);

  // Figure out how to round the bubble's four corners.
  SkScalar rad[8];

  // Top Edges - if the bubble is in its bottom position (sticking downwards),
  // then we square the top edges. Otherwise, we square the edges based on the
  // position of the bubble within the window (the bubble is positioned in the
  // southeast corner in RTL and in the southwest corner in LTR).
  if (style_ == STYLE_BOTTOM) {
    // Top Left corner.
    rad[0] = 0;
    rad[1] = 0;

    // Top Right corner.
    rad[2] = 0;
    rad[3] = 0;
  } else {
    if (base::i18n::IsRTL() != (style_ == STYLE_STANDARD_RIGHT)) {
      // The text is RtL or the bubble is on the right side (but not both).

      // Top Left corner.
      rad[0] = SkIntToScalar(kBubbleCornerRadius);
      rad[1] = SkIntToScalar(kBubbleCornerRadius);

      // Top Right corner.
      rad[2] = 0;
      rad[3] = 0;
    } else {
      // Top Left corner.
      rad[0] = 0;
      rad[1] = 0;

      // Top Right corner.
      rad[2] = SkIntToScalar(kBubbleCornerRadius);
      rad[3] = SkIntToScalar(kBubbleCornerRadius);
    }
  }

  // Bottom edges - square these off if the bubble is in its standard position
  // (sticking upward).
  if (style_ == STYLE_STANDARD || style_ == STYLE_STANDARD_RIGHT) {
    // Bottom Right Corner.
    rad[4] = 0;
    rad[5] = 0;

    // Bottom Left Corner.
    rad[6] = 0;
    rad[7] = 0;
  } else {
    // Bottom Right Corner.
    rad[4] = SkIntToScalar(kBubbleCornerRadius);
    rad[5] = SkIntToScalar(kBubbleCornerRadius);

    // Bottom Left Corner.
    rad[6] = SkIntToScalar(kBubbleCornerRadius);
    rad[7] = SkIntToScalar(kBubbleCornerRadius);
  }

  // Draw the bubble's shadow.
  int width = popup_bounds.width();
  int height = popup_bounds.height();
  SkRect rect;
  rect.set(0, 0,
           SkIntToScalar(width),
           SkIntToScalar(height));
  SkPath shadow_path;
  shadow_path.addRoundRect(rect, rad, SkPath::kCW_Direction);
  SkPaint shadow_paint;
  shadow_paint.setFlags(SkPaint::kAntiAlias_Flag);
  shadow_paint.setColor(kShadowColor);
  canvas->AsCanvasSkia()->drawPath(shadow_path, shadow_paint);

  // Draw the bubble.
  rect.set(SkIntToScalar(kShadowThickness),
           SkIntToScalar(kShadowThickness),
           SkIntToScalar(width - kShadowThickness),
           SkIntToScalar(height - kShadowThickness));
  SkPath path;
  path.addRoundRect(rect, rad, SkPath::kCW_Direction);
  canvas->AsCanvasSkia()->drawPath(path, paint);

  // Draw highlight text and then the text body. In order to make sure the text
  // is aligned to the right on RTL UIs, we mirror the text bounds if the
  // locale is RTL.
  int text_width = std::min(views::Label::font().GetStringWidth(text_),
      width - (kShadowThickness * 2) - kTextPositionX - kTextHorizPadding);
  int text_height = height - (kShadowThickness * 2);
  gfx::Rect body_bounds(kShadowThickness + kTextPositionX,
                        kShadowThickness,
                        std::max(0, text_width),
                        std::max(0, text_height));
  body_bounds.set_x(MirroredLeftPointForRect(body_bounds));
  SkColor text_color =
      theme_provider_->GetColor(BrowserThemeProvider::COLOR_TAB_TEXT);

  // DrawStringInt doesn't handle alpha, so we'll do the blending ourselves.
  text_color = SkColorSetARGB(
      SkColorGetA(text_color),
      (SkColorGetR(text_color) + SkColorGetR(toolbar_color)) / 2,
      (SkColorGetG(text_color) + SkColorGetR(toolbar_color)) / 2,
      (SkColorGetB(text_color) + SkColorGetR(toolbar_color)) / 2);
  canvas->DrawStringInt(text_,
                        views::Label::font(),
                        text_color,
                        body_bounds.x(),
                        body_bounds.y(),
                        body_bounds.width(),
                        body_bounds.height());
}

// StatusViewExpander ---------------------------------------------------------
// Manages the expansion and contraction of the status bubble as it accommodates
// URLs too long to fit in the standard bubble. Changes are passed through the
// StatusView to paint.
class StatusBubbleViews::StatusViewExpander : public LinearAnimation,
                                              public AnimationDelegate {
 public:
  StatusViewExpander(StatusBubbleViews* status_bubble,
                     StatusView* status_view)
      : ALLOW_THIS_IN_INITIALIZER_LIST(LinearAnimation(kFramerate, this)),
        status_bubble_(status_bubble),
        status_view_(status_view),
        expansion_start_(0),
        expansion_end_(0) {
  }

  // Manage the expansion of the bubble.
  void StartExpansion(std::wstring expanded_text, int current_width,
                      int expansion_end);

  // Set width of fully expanded bubble.
  void SetExpandedWidth(int expanded_width);

 private:
  // Animation functions.
  int GetCurrentBubbleWidth();
  void SetBubbleWidth(int width);
  void AnimateToState(double state);
  void AnimationEnded(const Animation* animation);

  // Manager that owns us.
  StatusBubbleViews* status_bubble_;

  // Change the bounds and text of this view.
  StatusView* status_view_;

  // Text elided (if needed) to fit maximum status bar width.
  std::wstring expanded_text_;

  // Widths at expansion start and end.
  int expansion_start_;
  int expansion_end_;
};

void StatusBubbleViews::StatusViewExpander::AnimateToState(double state) {
  SetBubbleWidth(GetCurrentBubbleWidth());
}

void StatusBubbleViews::StatusViewExpander::AnimationEnded(
    const Animation* animation) {
  SetBubbleWidth(expansion_end_);
  status_view_->SetText(expanded_text_, false);
}

void StatusBubbleViews::StatusViewExpander::StartExpansion(
    std::wstring expanded_text, int expansion_start,
    int expansion_end) {
  expanded_text_ = expanded_text;
  expansion_start_ = expansion_start;
  expansion_end_ = expansion_end;
  int min_duration = std::max(kMinExpansionStepDurationMS,
      static_cast<int>(kMaxExpansionStepDurationMS *
          (expansion_end - expansion_start) / 100.0));
  SetDuration(std::min(kMaxExpansionStepDurationMS, min_duration));
  Start();
}

int StatusBubbleViews::StatusViewExpander::GetCurrentBubbleWidth() {
  return static_cast<int>(expansion_start_ +
      (expansion_end_ - expansion_start_) * LinearAnimation::GetCurrentValue());
}

void StatusBubbleViews::StatusViewExpander::SetBubbleWidth(int width) {
  status_bubble_->SetBubbleWidth(width);
  status_view_->SchedulePaint();
}

// StatusBubble ---------------------------------------------------------------

const int StatusBubbleViews::kShadowThickness = 1;

StatusBubbleViews::StatusBubbleViews(views::Widget* frame)
    : offset_(0),
      popup_(NULL),
      opacity_(0),
      frame_(frame),
      view_(NULL),
      download_shelf_is_visible_(false),
      is_expanded_(false),
      ALLOW_THIS_IN_INITIALIZER_LIST(expand_timer_factory_(this)) {
  expand_view_.reset();
}

StatusBubbleViews::~StatusBubbleViews() {
  CancelExpandTimer();
  if (popup_.get())
    popup_->CloseNow();
}

void StatusBubbleViews::Init() {
  if (!popup_.get()) {
    popup_.reset(Widget::CreatePopupWidget(Widget::Transparent,
                                           Widget::NotAcceptEvents,
                                           Widget::NotDeleteOnDestroy,
                                           Widget::MirrorOriginInRTL));
    if (!view_)
      view_ = new StatusView(this, popup_.get(), frame_->GetThemeProvider());
    if (!expand_view_.get())
      expand_view_.reset(new StatusViewExpander(this, view_));
    popup_->SetOpacity(0x00);
    popup_->Init(frame_->GetNativeView(), gfx::Rect());
    popup_->SetContentsView(view_);
    Reposition();
    popup_->Show();
  }
}

void StatusBubbleViews::Reposition() {
  if (popup_.get()) {
    gfx::Point top_left;
    views::View::ConvertPointToScreen(frame_->GetRootView(), &top_left);

    popup_->SetBounds(gfx::Rect(top_left.x() + position_.x(),
                                top_left.y() + position_.y(),
                                size_.width(), size_.height()));
  }
}

gfx::Size StatusBubbleViews::GetPreferredSize() {
  return gfx::Size(0, ResourceBundle::GetSharedInstance().GetFont(
      ResourceBundle::BaseFont).height() + kTotalVerticalPadding);
}

void StatusBubbleViews::SetBounds(int x, int y, int w, int h) {
  // If the UI layout is RTL, we need to mirror the position of the bubble
  // relative to the parent.
  if (base::i18n::IsRTL()) {
    gfx::Rect frame_bounds;
    frame_->GetBounds(&frame_bounds, false);
    int mirrored_x = frame_bounds.width() - x - w;
    position_.SetPoint(mirrored_x, y);
  } else {
    position_.SetPoint(x, y);
  }

  size_.SetSize(w, h);
  Reposition();
}

void StatusBubbleViews::SetStatus(const std::wstring& status_text) {
  if (size_.IsEmpty())
    return;  // We have no bounds, don't attempt to show the popup.

  if (status_text_ == status_text && !status_text.empty())
    return;

  if (!IsFrameVisible())
    return;  // Don't show anything if the parent isn't visible.

  Init();
  status_text_ = status_text;
  if (!status_text_.empty()) {
    view_->SetText(status_text, true);
    view_->Show();
  } else if (!url_text_.empty()) {
    view_->SetText(url_text_, true);
  } else {
    view_->SetText(std::wstring(), true);
  }
}

void StatusBubbleViews::SetURL(const GURL& url, const std::wstring& languages) {
  languages_ = languages;
  url_ = url;
  if (size_.IsEmpty())
    return;  // We have no bounds, don't attempt to show the popup.

  Init();

  // If we want to clear a displayed URL but there is a status still to
  // display, display that status instead.
  if (url.is_empty() && !status_text_.empty()) {
    url_text_ = std::wstring();
    if (IsFrameVisible())
      view_->SetText(status_text_, true);
    return;
  }

  // Reset expansion state only when bubble is completely hidden.
  if (view_->GetState() == StatusView::BUBBLE_HIDDEN) {
    is_expanded_ = false;
    SetBubbleWidth(GetStandardStatusBubbleWidth());
  }

  // Set Elided Text corresponding to the GURL object.
  gfx::Rect popup_bounds;
  popup_->GetBounds(&popup_bounds, true);
  int text_width = static_cast<int>(popup_bounds.width() -
      (kShadowThickness * 2) - kTextPositionX - kTextHorizPadding - 1);
  url_text_ = gfx::ElideUrl(url, view_->Label::font(), text_width,
      languages);

  std::wstring original_url_text = net::FormatUrl(url, languages);

  // An URL is always treated as a left-to-right string. On right-to-left UIs
  // we need to explicitly mark the URL as LTR to make sure it is displayed
  // correctly.
  base::i18n::GetDisplayStringInLTRDirectionality(&url_text_);

  if (IsFrameVisible()) {
    view_->SetText(url_text_, true);

    CancelExpandTimer();

    // If bubble is already in expanded state, shift to adjust to new text
    // size (shrinking or expanding). Otherwise delay.
    if (is_expanded_ && !url.is_empty())
      ExpandBubble();
    else if (original_url_text.length() > url_text_.length())
      MessageLoop::current()->PostDelayedTask(FROM_HERE,
          expand_timer_factory_.NewRunnableMethod(
          &StatusBubbleViews::ExpandBubble), kExpandHoverDelay);
  }
}

void StatusBubbleViews::Hide() {
  status_text_ = std::wstring();
  url_text_ = std::wstring();
  if (view_)
    view_->Hide();
}

void StatusBubbleViews::MouseMoved(const gfx::Point& location,
                                   bool left_content) {
  if (left_content)
    return;

  if (view_) {
    view_->ResetTimer();

    if (view_->GetState() != StatusView::BUBBLE_HIDDEN &&
        view_->GetState() != StatusView::BUBBLE_HIDING_FADE &&
        view_->GetState() != StatusView::BUBBLE_HIDING_TIMER) {
      AvoidMouse(location);
    }
  }
}

void StatusBubbleViews::UpdateDownloadShelfVisibility(bool visible) {
  download_shelf_is_visible_ = visible;
}

void StatusBubbleViews::AvoidMouse(const gfx::Point& location) {
  // Get the position of the frame.
  gfx::Point top_left;
  views::RootView* root = frame_->GetRootView();
  views::View::ConvertPointToScreen(root, &top_left);
  int window_width = root->GetLocalBounds(true).width();  // border included.

  // Get the cursor position relative to the popup.
  gfx::Point relative_location = location;
  if (base::i18n::IsRTL()) {
    int top_right_x = top_left.x() + window_width;
    relative_location.set_x(top_right_x - relative_location.x());
  } else {
    relative_location.set_x(
        relative_location.x() - (top_left.x() + position_.x()));
  }
  relative_location.set_y(
      relative_location.y() - (top_left.y() + position_.y()));

  // If the mouse is in a position where we think it would move the
  // status bubble, figure out where and how the bubble should be moved.
  if (relative_location.y() > -kMousePadding &&
      relative_location.x() < size_.width() + kMousePadding) {
    int offset = kMousePadding + relative_location.y();

    // Make the movement non-linear.
    offset = offset * offset / kMousePadding;

    // When the mouse is entering from the right, we want the offset to be
    // scaled by how horizontally far away the cursor is from the bubble.
    if (relative_location.x() > size_.width()) {
      offset = static_cast<int>(static_cast<float>(offset) * (
          static_cast<float>(kMousePadding -
              (relative_location.x() - size_.width())) /
          static_cast<float>(kMousePadding)));
    }

    // Cap the offset and change the visual presentation of the bubble
    // depending on where it ends up (so that rounded corners square off
    // and mate to the edges of the tab content).
    if (offset >= size_.height() - kShadowThickness * 2) {
      offset = size_.height() - kShadowThickness * 2;
      view_->SetStyle(StatusView::STYLE_BOTTOM);
    } else if (offset > kBubbleCornerRadius / 2 - kShadowThickness) {
      view_->SetStyle(StatusView::STYLE_FLOATING);
    } else {
      view_->SetStyle(StatusView::STYLE_STANDARD);
    }

    // Check if the bubble sticks out from the monitor or will obscure
    // download shelf.
    gfx::NativeView widget = frame_->GetNativeView();
    gfx::Rect monitor_rect =
        views::Screen::GetMonitorWorkAreaNearestWindow(widget);
    const int bubble_bottom_y = top_left.y() + position_.y() + size_.height();

    if (bubble_bottom_y + offset > monitor_rect.height() ||
        (download_shelf_is_visible_ &&
         (view_->GetStyle() == StatusView::STYLE_FLOATING ||
          view_->GetStyle() == StatusView::STYLE_BOTTOM))) {
      // The offset is still too large. Move the bubble to the right and reset
      // Y offset_ to zero.
      view_->SetStyle(StatusView::STYLE_STANDARD_RIGHT);
      offset_ = 0;

      // Subtract border width + bubble width.
      int right_position_x = window_width - (position_.x() + size_.width());
      popup_->SetBounds(gfx::Rect(top_left.x() + right_position_x,
                                  top_left.y() + position_.y(),
                                  size_.width(), size_.height()));
    } else {
      offset_ = offset;
      popup_->SetBounds(gfx::Rect(top_left.x() + position_.x(),
                                  top_left.y() + position_.y() + offset_,
                                  size_.width(), size_.height()));
    }
  } else if (offset_ != 0 ||
      view_->GetStyle() == StatusView::STYLE_STANDARD_RIGHT) {
    offset_ = 0;
    view_->SetStyle(StatusView::STYLE_STANDARD);
    popup_->SetBounds(gfx::Rect(top_left.x() + position_.x(),
                                top_left.y() + position_.y(),
                                size_.width(), size_.height()));
  }
}

bool StatusBubbleViews::IsFrameVisible() {
  if (!frame_->IsVisible())
    return false;

  views::Window* window = frame_->GetWindow();
  return !window || !window->IsMinimized();
}

void StatusBubbleViews::ExpandBubble() {
  // Elide URL to maximum possible size, then check actual length (it may
  // still be too long to fit) before expanding bubble.
  gfx::Rect popup_bounds;
  popup_->GetBounds(&popup_bounds, true);
  int max_status_bubble_width = GetMaxStatusBubbleWidth();
  url_text_ = gfx::ElideUrl(url_, view_->Label::font(),
                            max_status_bubble_width, languages_);
  int expanded_bubble_width =
      std::max(GetStandardStatusBubbleWidth(),
               std::min(view_->Label::font().GetStringWidth(url_text_) +
                            (kShadowThickness * 2) + kTextPositionX +
                            kTextHorizPadding + 1,
                        max_status_bubble_width));
  is_expanded_ = true;
  expand_view_->StartExpansion(url_text_, popup_bounds.width(),
                               expanded_bubble_width);
}

int StatusBubbleViews::GetStandardStatusBubbleWidth() {
  gfx::Rect frame_bounds;
  frame_->GetBounds(&frame_bounds, false);
  return frame_bounds.width() / 3;
}

int StatusBubbleViews::GetMaxStatusBubbleWidth() {
  gfx::Rect frame_bounds;
  frame_->GetBounds(&frame_bounds, false);
  return static_cast<int>(frame_bounds.width() - (kShadowThickness * 2) -
      kTextPositionX - kTextHorizPadding - 1 -
      views::NativeScrollBar::GetVerticalScrollBarWidth());
}

void StatusBubbleViews::SetBubbleWidth(int width) {
  size_.set_width(width);
  SetBounds(position_.x(), position_.y(), size_.width(), size_.height());
}

void StatusBubbleViews::CancelExpandTimer() {
  if (!expand_timer_factory_.empty())
    expand_timer_factory_.RevokeAll();
}