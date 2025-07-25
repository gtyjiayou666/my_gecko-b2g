
/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsBaseWidget.h"

#include <utility>

#include "GLConsts.h"
#include "InputData.h"
#include "LiveResizeListener.h"
#include "SwipeTracker.h"
#include "TouchEvents.h"
#include "X11UndefineNone.h"
#include "base/thread.h"
#include "mozilla/ArrayUtils.h"
#include "mozilla/Attributes.h"
#include "mozilla/GlobalKeyListener.h"
#include "mozilla/IMEStateManager.h"
#include "mozilla/Logging.h"
#include "mozilla/MouseEvents.h"
#include "mozilla/NativeKeyBindingsType.h"
#include "mozilla/Preferences.h"
#include "mozilla/PresShell.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Sprintf.h"
#include "mozilla/StaticPrefs_apz.h"
#include "mozilla/StaticPrefs_dom.h"
#include "mozilla/StaticPrefs_gfx.h"
#include "mozilla/StaticPrefs_layers.h"
#include "mozilla/StaticPrefs_layout.h"
#include "mozilla/TextEventDispatcher.h"
#include "mozilla/TextEventDispatcherListener.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Unused.h"
#include "mozilla/VsyncDispatcher.h"
#include "mozilla/dom/BrowserParent.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/SimpleGestureEventBinding.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/GPUProcessManager.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/layers/APZCCallbackHelper.h"
#include "mozilla/layers/TouchActionHelper.h"
#include "mozilla/layers/APZEventState.h"
#include "mozilla/layers/APZInputBridge.h"
#include "mozilla/layers/APZThreadUtils.h"
#include "mozilla/layers/ChromeProcessController.h"
#include "mozilla/layers/Compositor.h"
#include "mozilla/layers/CompositorBridgeChild.h"
#include "mozilla/layers/CompositorBridgeParent.h"
#include "mozilla/layers/CompositorOptions.h"
#include "mozilla/layers/IAPZCTreeManager.h"
#include "mozilla/layers/ImageBridgeChild.h"
#include "mozilla/layers/InputAPZContext.h"
#include "mozilla/layers/WebRenderLayerManager.h"
#include "mozilla/webrender/WebRenderTypes.h"
#include "mozilla/widget/ScreenManager.h"
#include "nsAppDirectoryServiceDefs.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "nsDeviceContext.h"
#include "nsGfxCIID.h"
#include "nsIAppWindow.h"
#include "nsIBaseWindow.h"
#include "nsIContent.h"
#include "nsIScreenManager.h"
#include "nsISimpleEnumerator.h"
#include "nsIWidgetListener.h"
#include "nsRefPtrHashtable.h"
#include "nsServiceManagerUtils.h"
#include "nsWidgetsCID.h"
#include "nsXULPopupManager.h"
#include "prdtoa.h"
#include "prenv.h"
#ifdef ACCESSIBILITY
#  include "nsAccessibilityService.h"
#endif
#include "gfxConfig.h"
#include "gfxUtils.h"  // for ToDeviceColor
#include "mozilla/layers/CompositorSession.h"
#include "VRManagerChild.h"
#include "gfxConfig.h"
#include "nsView.h"
#include "nsViewManager.h"
#include <iostream>
static mozilla::LazyLogModule sBaseWidgetLog("BaseWidget");

#ifdef DEBUG
#  include "nsIObserver.h"

static void debug_RegisterPrefCallbacks();

#endif

#ifdef NOISY_WIDGET_LEAKS
static int32_t gNumWidgets;
#endif

using namespace mozilla::dom;
using namespace mozilla::layers;
using namespace mozilla::ipc;
using namespace mozilla::widget;
using namespace mozilla;

// Async pump timer during injected long touch taps
#define TOUCH_INJECT_PUMP_TIMER_MSEC 50
#define TOUCH_INJECT_LONG_TAP_DEFAULT_MSEC 1500
int32_t nsIWidget::sPointerIdCounter = 0;

// Some statics from nsIWidget.h
/*static*/
uint64_t AutoObserverNotifier::sObserverId = 0;
/*static*/ nsTHashMap<uint64_t, nsCOMPtr<nsIObserver>>
    AutoObserverNotifier::sSavedObservers;

// The maximum amount of time to let the EnableDragDrop runnable wait in the
// idle queue before timing out and moving it to the regular queue. Value is in
// milliseconds.
const uint32_t kAsyncDragDropTimeout = 1000;

NS_IMPL_ISUPPORTS(nsBaseWidget, nsIWidget, nsISupportsWeakReference)

//-------------------------------------------------------------------------
//
// nsBaseWidget constructor
//
//-------------------------------------------------------------------------

nsBaseWidget::nsBaseWidget() : nsBaseWidget(BorderStyle::None) {}

nsBaseWidget::nsBaseWidget(BorderStyle aBorderStyle)
    : mWidgetListener(nullptr),
      mAttachedWidgetListener(nullptr),
      mPreviouslyAttachedWidgetListener(nullptr),
      mCompositorVsyncDispatcher(nullptr),
      mBorderStyle(aBorderStyle),
      mBounds(0, 0, 0, 0),
      mIsTiled(false),
      mPopupLevel(PopupLevel::Top),
      mPopupType(PopupType::Any),
      mHasRemoteContent(false),
      mUpdateCursor(true),
      mUseAttachedEvents(false),
      mIMEHasFocus(false),
      mIMEHasQuit(false),
      mIsFullyOccluded(false),
      mNeedFastSnaphot(false),
      mCurrentPanGestureBelongsToSwipe(false),
      mIsPIPWindow(false) {
#ifdef NOISY_WIDGET_LEAKS
  gNumWidgets++;
  printf("WIDGETS+ = %d\n", gNumWidgets);
#endif

#ifdef DEBUG
  debug_RegisterPrefCallbacks();
#endif

  mShutdownObserver = new WidgetShutdownObserver(this);
}

NS_IMPL_ISUPPORTS(WidgetShutdownObserver, nsIObserver)

WidgetShutdownObserver::WidgetShutdownObserver(nsBaseWidget* aWidget)
    : mWidget(aWidget), mRegistered(false) {
  Register();
}

WidgetShutdownObserver::~WidgetShutdownObserver() {
  // No need to call Unregister(), we can't be destroyed until nsBaseWidget
  // gets torn down. The observer service and nsBaseWidget have a ref on us
  // so nsBaseWidget has to call Unregister and then clear its ref.
}

NS_IMETHODIMP
WidgetShutdownObserver::Observe(nsISupports* aSubject, const char* aTopic,
                                const char16_t* aData) {
  if (!mWidget) {
    return NS_OK;
  }
  if (!strcmp(aTopic, NS_XPCOM_SHUTDOWN_OBSERVER_ID)) {
    RefPtr<nsBaseWidget> widget(mWidget);
    widget->Shutdown();
  } else if (!strcmp(aTopic, "quit-application")) {
    RefPtr<nsBaseWidget> widget(mWidget);
    widget->QuitIME();
  }
  return NS_OK;
}

void WidgetShutdownObserver::Register() {
  if (!mRegistered) {
    mRegistered = true;
    nsContentUtils::RegisterShutdownObserver(this);

#ifndef MOZ_WIDGET_ANDROID
    // The primary purpose of observing quit-application is
    // to avoid leaking a widget on Windows when nothing else
    // breaks the circular reference between the widget and
    // TSFTextStore. However, our Android IME code crashes if
    // doing this on Android, so let's not do this on Android.
    // Doing this on Gtk and Mac just in case.
    nsCOMPtr<nsIObserverService> observerService =
        mozilla::services::GetObserverService();
    if (observerService) {
      observerService->AddObserver(this, "quit-application", false);
    }
#endif
  }
}

void WidgetShutdownObserver::Unregister() {
  if (mRegistered) {
    mWidget = nullptr;

#ifndef MOZ_WIDGET_ANDROID
    nsCOMPtr<nsIObserverService> observerService =
        mozilla::services::GetObserverService();
    if (observerService) {
      observerService->RemoveObserver(this, "quit-application");
    }
#endif

    nsContentUtils::UnregisterShutdownObserver(this);
    mRegistered = false;
  }
}

#define INTL_APP_LOCALES_CHANGED "intl:app-locales-changed"

NS_IMPL_ISUPPORTS(LocalesChangedObserver, nsIObserver)

LocalesChangedObserver::LocalesChangedObserver(nsBaseWidget* aWidget)
    : mWidget(aWidget), mRegistered(false) {
  Register();
}

LocalesChangedObserver::~LocalesChangedObserver() {
  // No need to call Unregister(), we can't be destroyed until nsBaseWidget
  // gets torn down. The observer service and nsBaseWidget have a ref on us
  // so nsBaseWidget has to call Unregister and then clear its ref.
}

NS_IMETHODIMP
LocalesChangedObserver::Observe(nsISupports* aSubject, const char* aTopic,
                                const char16_t* aData) {
  if (!mWidget) {
    return NS_OK;
  }
  if (!strcmp(aTopic, INTL_APP_LOCALES_CHANGED)) {
    RefPtr<nsBaseWidget> widget(mWidget);
    widget->LocalesChanged();
  }
  return NS_OK;
}

void LocalesChangedObserver::Register() {
  if (mRegistered) {
    return;
  }

  nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
  if (obs) {
    obs->AddObserver(this, INTL_APP_LOCALES_CHANGED, true);
  }

  // Locale might be update before registering
  RefPtr<nsBaseWidget> widget(mWidget);
  widget->LocalesChanged();

  mRegistered = true;
}

void LocalesChangedObserver::Unregister() {
  if (!mRegistered) {
    return;
  }

  nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
  if (obs) {
    obs->RemoveObserver(this, INTL_APP_LOCALES_CHANGED);
  }

  mWidget = nullptr;
  mRegistered = false;
}

void nsBaseWidget::Shutdown() {
  NotifyLiveResizeStopped();
  DestroyCompositor();
  FreeLocalesChangedObserver();
  FreeShutdownObserver();
}

void nsBaseWidget::QuitIME() {
  IMEStateManager::WidgetOnQuit(this);
  this->mIMEHasQuit = true;
}

void nsBaseWidget::DestroyCompositor() {
  RevokeTransactionIdAllocator();

  // We release this before releasing the compositor, since it may hold the
  // last reference to our ClientLayerManager. ClientLayerManager's dtor can
  // trigger a paint, creating a new compositor, and we don't want to re-use
  // the old vsync dispatcher.
  if (mCompositorVsyncDispatcher) {
    MOZ_ASSERT(mCompositorVsyncDispatcherLock.get());

    MutexAutoLock lock(*mCompositorVsyncDispatcherLock.get());
    mCompositorVsyncDispatcher->Shutdown();
    mCompositorVsyncDispatcher = nullptr;
  }

  // The compositor shutdown sequence looks like this:
  //  1. CompositorSession calls CompositorBridgeChild::Destroy.
  //  2. CompositorBridgeChild synchronously sends WillClose.
  //  3. CompositorBridgeParent releases some resources (such as the layer
  //     manager, compositor, and widget).
  //  4. CompositorBridgeChild::Destroy returns.
  //  5. Asynchronously, CompositorBridgeParent::ActorDestroy will fire on the
  //     compositor thread when the I/O thread closes the IPC channel.
  //  6. Step 5 will schedule DeferredDestroy on the compositor thread, which
  //     releases the reference CompositorBridgeParent holds to itself.
  //
  // When CompositorSession::Shutdown returns, we assume the compositor is gone
  // or will be gone very soon.
  if (mCompositorSession) {
    ReleaseContentController();
    mAPZC = nullptr;
    SetCompositorWidgetDelegate(nullptr);
    mCompositorBridgeChild = nullptr;
    mCompositorSession->Shutdown();
    mCompositorSession = nullptr;
  }
}

// This prevents the layer manager from starting a new transaction during
// shutdown.
void nsBaseWidget::RevokeTransactionIdAllocator() {
  if (!mWindowRenderer || !mWindowRenderer->AsWebRender()) {
    return;
  }
  mWindowRenderer->AsWebRender()->SetTransactionIdAllocator(nullptr);
}

void nsBaseWidget::ReleaseContentController() {
  if (mRootContentController) {
    mRootContentController->Destroy();
    mRootContentController = nullptr;
  }
}

void nsBaseWidget::DestroyLayerManager() {
  if (mWindowRenderer) {
    mWindowRenderer->Destroy();
    mWindowRenderer = nullptr;
  }
  DestroyCompositor();
}

void nsBaseWidget::OnRenderingDeviceReset() { DestroyLayerManager(); }

void nsBaseWidget::FreeShutdownObserver() {
  if (mShutdownObserver) {
    mShutdownObserver->Unregister();
  }
  mShutdownObserver = nullptr;
}

void nsBaseWidget::FreeLocalesChangedObserver() {
  if (mLocalesChangedObserver) {
    mLocalesChangedObserver->Unregister();
  }
  mLocalesChangedObserver = nullptr;
}

//-------------------------------------------------------------------------
//
// nsBaseWidget destructor
//
//-------------------------------------------------------------------------

nsBaseWidget::~nsBaseWidget() {
  if (mSwipeTracker) {
    mSwipeTracker->Destroy();
    mSwipeTracker = nullptr;
  }

  IMEStateManager::WidgetDestroyed(this);

  FreeLocalesChangedObserver();
  FreeShutdownObserver();
  DestroyLayerManager();

#ifdef NOISY_WIDGET_LEAKS
  gNumWidgets--;
  printf("WIDGETS- = %d\n", gNumWidgets);
#endif
}

//-------------------------------------------------------------------------
//
// Basic create.
//
//-------------------------------------------------------------------------
void nsBaseWidget::BaseCreate(nsIWidget* aParent, widget::InitData* aInitData) {
  if (aInitData) {
    mWindowType = aInitData->mWindowType;
    mBorderStyle = aInitData->mBorderStyle;
    mPopupLevel = aInitData->mPopupLevel;
    mPopupType = aInitData->mPopupHint;
    mHasRemoteContent = aInitData->mHasRemoteContent;
    mIsPIPWindow = aInitData->mPIPWindow;
  }

  if (aParent) {
    aParent->AddChild(this);
  }
}

//-------------------------------------------------------------------------
//
// Accessor functions to get/set the client data
//
//-------------------------------------------------------------------------

nsIWidgetListener* nsBaseWidget::GetWidgetListener() const {
  return mWidgetListener;
}

void nsBaseWidget::SetWidgetListener(nsIWidgetListener* aWidgetListener) {
  mWidgetListener = aWidgetListener;
}

already_AddRefed<nsIWidget> nsBaseWidget::CreateChild(
    const LayoutDeviceIntRect& aRect, widget::InitData* aInitData,
    bool aForceUseIWidgetParent) {
  nsIWidget* parent = this;
  nsNativeWidget nativeParent = nullptr;

  if (!aForceUseIWidgetParent) {
    // Use only either parent or nativeParent, not both, to match
    // existing code.  Eventually Create() should be divested of its
    // nativeWidget parameter.
    nativeParent = parent ? parent->GetNativeData(NS_NATIVE_WIDGET) : nullptr;
    parent = nativeParent ? nullptr : parent;
    MOZ_ASSERT(!parent || !nativeParent, "messed up logic");
  }

  nsCOMPtr<nsIWidget> widget;
  if (aInitData && aInitData->mWindowType == WindowType::Popup) {
    widget = AllocateChildPopupWidget();
  } else {
    widget = nsIWidget::CreateChildWindow();
  }

  if (widget && mNeedFastSnaphot) {
    widget->SetNeedFastSnaphot();
  }

  if (widget &&
      NS_SUCCEEDED(widget->Create(parent, nativeParent, aRect, aInitData))) {
    return widget.forget();
  }

  return nullptr;
}

// Attach a view to our widget which we'll send events to.
void nsBaseWidget::AttachViewToTopLevel(bool aUseAttachedEvents) {
  NS_ASSERTION((mWindowType == WindowType::TopLevel ||
                mWindowType == WindowType::Dialog ||
                mWindowType == WindowType::Invisible ||
                mWindowType == WindowType::Child),
               "Can't attach to window of that type");

  mUseAttachedEvents = aUseAttachedEvents;
}

nsIWidgetListener* nsBaseWidget::GetAttachedWidgetListener() const {
  return mAttachedWidgetListener;
}

nsIWidgetListener* nsBaseWidget::GetPreviouslyAttachedWidgetListener() {
  return mPreviouslyAttachedWidgetListener;
}

void nsBaseWidget::SetPreviouslyAttachedWidgetListener(
    nsIWidgetListener* aListener) {
  mPreviouslyAttachedWidgetListener = aListener;
}

void nsBaseWidget::SetAttachedWidgetListener(nsIWidgetListener* aListener) {
  mAttachedWidgetListener = aListener;
}

//-------------------------------------------------------------------------
//
// Close this nsBaseWidget
//
//-------------------------------------------------------------------------
void nsBaseWidget::Destroy() {
  DestroyCompositor();

  // Just in case our parent is the only ref to us
  nsCOMPtr<nsIWidget> kungFuDeathGrip(this);
  // disconnect from the parent
  nsIWidget* parent = GetParent();
  if (parent) {
    parent->RemoveChild(this);
  }
}

//-------------------------------------------------------------------------
//
// Get this nsBaseWidget parent
//
//-------------------------------------------------------------------------
nsIWidget* nsBaseWidget::GetParent(void) { return nullptr; }

//-------------------------------------------------------------------------
//
// Get this nsBaseWidget top level widget
//
//-------------------------------------------------------------------------
nsIWidget* nsBaseWidget::GetTopLevelWidget() {
  nsIWidget *topLevelWidget = nullptr, *widget = this;
  while (widget) {
    topLevelWidget = widget;
    widget = widget->GetParent();
  }
  return topLevelWidget;
}

//-------------------------------------------------------------------------
//
// Get this nsBaseWidget's top (non-sheet) parent (if it's a sheet)
//
//-------------------------------------------------------------------------
nsIWidget* nsBaseWidget::GetSheetWindowParent(void) { return nullptr; }

float nsBaseWidget::GetDPI() { return 96.0f; }

CSSToLayoutDeviceScale nsIWidget::GetDefaultScale() {
  double devPixelsPerCSSPixel = StaticPrefs::layout_css_devPixelsPerPx();

  if (devPixelsPerCSSPixel <= 0.0) {
    devPixelsPerCSSPixel = GetDefaultScaleInternal();
  }

  return CSSToLayoutDeviceScale(devPixelsPerCSSPixel);
}

nsIntSize nsIWidget::CustomCursorSize(const Cursor& aCursor) {
  MOZ_ASSERT(aCursor.IsCustom());
  int32_t width = 0;
  int32_t height = 0;
  aCursor.mContainer->GetWidth(&width);
  aCursor.mContainer->GetHeight(&height);
  aCursor.mResolution.ApplyTo(width, height);
  return {width, height};
}

LayoutDeviceIntSize nsIWidget::ClientToWindowSizeDifference() {
  auto margin = ClientToWindowMargin();
  MOZ_ASSERT(margin.top >= 0, "Window should be bigger than client area");
  MOZ_ASSERT(margin.left >= 0, "Window should be bigger than client area");
  MOZ_ASSERT(margin.right >= 0, "Window should be bigger than client area");
  MOZ_ASSERT(margin.bottom >= 0, "Window should be bigger than client area");
  return {margin.LeftRight(), margin.TopBottom()};
}

RefPtr<mozilla::VsyncDispatcher> nsIWidget::GetVsyncDispatcher() {
  return nullptr;
}

//-------------------------------------------------------------------------
//
// Add a child to the list of children
//
//-------------------------------------------------------------------------
void nsBaseWidget::AddChild(nsIWidget* aChild) {
  MOZ_ASSERT(!aChild->GetNextSibling() && !aChild->GetPrevSibling(),
             "aChild not properly removed from its old child list");

  if (!mFirstChild) {
    mFirstChild = mLastChild = aChild;
  } else {
    // append to the list
    MOZ_ASSERT(mLastChild);
    MOZ_ASSERT(!mLastChild->GetNextSibling());
    mLastChild->SetNextSibling(aChild);
    aChild->SetPrevSibling(mLastChild);
    mLastChild = aChild;
  }
}

//-------------------------------------------------------------------------
//
// Remove a child from the list of children
//
//-------------------------------------------------------------------------
void nsBaseWidget::RemoveChild(nsIWidget* aChild) {
#ifdef DEBUG
#  ifdef XP_MACOSX
  // nsCocoaWindow doesn't implement GetParent, so in that case parent will be
  // null and we'll just have to do without this assertion.
  nsIWidget* parent = aChild->GetParent();
  NS_ASSERTION(!parent || parent == this, "Not one of our kids!");
#  else
  MOZ_RELEASE_ASSERT(aChild->GetParent() == this, "Not one of our kids!");
#  endif
#endif

  if (mLastChild == aChild) {
    mLastChild = mLastChild->GetPrevSibling();
  }
  if (mFirstChild == aChild) {
    mFirstChild = mFirstChild->GetNextSibling();
  }

  // Now remove from the list.  Make sure that we pass ownership of the tail
  // of the list correctly before we have aChild let go of it.
  nsIWidget* prev = aChild->GetPrevSibling();
  nsIWidget* next = aChild->GetNextSibling();
  if (prev) {
    prev->SetNextSibling(next);
  }
  if (next) {
    next->SetPrevSibling(prev);
  }

  aChild->SetNextSibling(nullptr);
  aChild->SetPrevSibling(nullptr);
}

//-------------------------------------------------------------------------
//
// Sets widget's position within its parent's child list.
//
//-------------------------------------------------------------------------
void nsBaseWidget::SetZIndex(int32_t aZIndex) {
  // Hold a ref to ourselves just in case, since we're going to remove
  // from our parent.
  nsCOMPtr<nsIWidget> kungFuDeathGrip(this);

  mZIndex = aZIndex;

  // reorder this child in its parent's list.
  auto* parent = static_cast<nsBaseWidget*>(GetParent());
  if (parent) {
    parent->RemoveChild(this);
    // Scope sib outside the for loop so we can check it afterward
    nsIWidget* sib = parent->GetFirstChild();
    for (; sib; sib = sib->GetNextSibling()) {
      int32_t childZIndex = GetZIndex();
      if (aZIndex < childZIndex) {
        // Insert ourselves before sib
        nsIWidget* prev = sib->GetPrevSibling();
        mNextSibling = sib;
        mPrevSibling = prev;
        sib->SetPrevSibling(this);
        if (prev) {
          prev->SetNextSibling(this);
        } else {
          NS_ASSERTION(sib == parent->mFirstChild, "Broken child list");
          // We've taken ownership of sib, so it's safe to have parent let
          // go of it
          parent->mFirstChild = this;
        }
        PlaceBehind(eZPlacementBelow, sib, false);
        break;
      }
    }
    // were we added to the list?
    if (!sib) {
      parent->AddChild(this);
    }
  }
}

void nsBaseWidget::GetWorkspaceID(nsAString& workspaceID) {
  workspaceID.Truncate();
}

void nsBaseWidget::MoveToWorkspace(const nsAString& workspaceID) {
  // Noop.
}

//-------------------------------------------------------------------------
//
// Get this component cursor
//
//-------------------------------------------------------------------------

void nsBaseWidget::SetCursor(const Cursor& aCursor) { mCursor = aCursor; }

//-------------------------------------------------------------------------
//
// Window transparency methods
//
//-------------------------------------------------------------------------

void nsBaseWidget::SetTransparencyMode(TransparencyMode aMode) {}

TransparencyMode nsBaseWidget::GetTransparencyMode() {
  return TransparencyMode::Opaque;
}

/* virtual */
void nsBaseWidget::PerformFullscreenTransition(FullscreenTransitionStage aStage,
                                               uint16_t aDuration,
                                               nsISupports* aData,
                                               nsIRunnable* aCallback) {
  MOZ_ASSERT_UNREACHABLE(
      "Should never call PerformFullscreenTransition on nsBaseWidget");
}

//-------------------------------------------------------------------------
//
// Put the window into full-screen mode
//
//-------------------------------------------------------------------------
void nsBaseWidget::InfallibleMakeFullScreen(bool aFullScreen) {
#define MOZ_FORMAT_RECT(fmtstr) "[" fmtstr "," fmtstr " " fmtstr "x" fmtstr "]"
#define MOZ_SPLAT_RECT(rect) \
  (rect).X(), (rect).Y(), (rect).Width(), (rect).Height()

  // Windows which can be made fullscreen are exactly those which are located on
  // the desktop, rather than being a child of some other window.
  MOZ_DIAGNOSTIC_ASSERT(BoundsUseDesktopPixels(),
                        "non-desktop windows cannot be made fullscreen");

  // Ensure that the OS chrome is hidden/shown before we resize and/or exit the
  // function.
  //
  // HideWindowChrome() may (depending on platform, implementation details, and
  // OS-level user preferences) alter the reported size of the window. The
  // obvious and principled solution is socks-and-shoes:
  //   - On entering fullscreen mode: hide window chrome, then perform resize.
  //   - On leaving fullscreen mode: unperform resize, then show window chrome.
  //
  // ... unfortunately, HideWindowChrome() requires Resize() to be called
  // afterwards (see bug 498835), which prevents this from being done in a
  // straightforward way.
  //
  // Instead, we always call HideWindowChrome() just before we call Resize().
  // This at least ensures that our measurements are consistently taken in a
  // pre-transition state.
  //
  // ... unfortunately again, coupling HideWindowChrome() to Resize() means that
  // we have to worry about the possibility of control flows that don't call
  // Resize() at all. (That shouldn't happen, but it's not trivial to rule out.)
  // We therefore set up a fallback to fix up the OS chrome if it hasn't been
  // done at exit time.
  bool hasAdjustedOSChrome = false;
  const auto adjustOSChrome = [&]() {
    if (hasAdjustedOSChrome) {
      MOZ_ASSERT_UNREACHABLE("window chrome should only be adjusted once");
      return;
    }
    HideWindowChrome(aFullScreen);
    hasAdjustedOSChrome = true;
  };
  const auto adjustChromeOnScopeExit = MakeScopeExit([&]() {
    if (hasAdjustedOSChrome) {
      return;
    }

    MOZ_LOG(sBaseWidgetLog, LogLevel::Warning,
            ("window was not resized within InfallibleMakeFullScreen()"));

    // Hide chrome and "resize" the window to its current size.
    auto rect = GetBounds();
    adjustOSChrome();
    Resize(rect.X(), rect.Y(), rect.Width(), rect.Height(), true);
  });

  // Attempt to resize to `rect`.
  //
  // Returns the actual rectangle resized to. (This may differ from `rect`, if
  // the OS is unhappy with it. See bug 1786226.)
  const auto doReposition = [&](auto rect) -> void {
    static_assert(std::is_base_of_v<DesktopPixel,
                                    std::remove_reference_t<decltype(rect)>>,
                  "doReposition requires a rectangle using desktop pixels");

    if (MOZ_LOG_TEST(sBaseWidgetLog, LogLevel::Debug)) {
      const DesktopRect previousSize =
          GetScreenBounds() / GetDesktopToDeviceScale();
      MOZ_LOG(sBaseWidgetLog, LogLevel::Debug,
              ("before resize: " MOZ_FORMAT_RECT("%f"),
               MOZ_SPLAT_RECT(previousSize)));
    }

    adjustOSChrome();
    Resize(rect.X(), rect.Y(), rect.Width(), rect.Height(), true);

    if (MOZ_LOG_TEST(sBaseWidgetLog, LogLevel::Warning)) {
      // `rect` may have any underlying data type; coerce to float to
      // simplify printf-style logging
      const gfx::RectTyped<DesktopPixel, float> rectAsFloat{rect};

      // The OS may have objected to the target position. That's not necessarily
      // a problem -- it'll happen regularly on Macs with camera notches in the
      // monitor, for instance (see bug 1786226) -- but it probably deserves to
      // be called out.
      //
      // Since there's floating-point math involved, the actual values may be
      // off by a few ulps -- as an upper bound, perhaps 8 * FLT_EPSILON *
      // max(MOZ_SPLAT_RECT(rect)) -- but 0.01 should be several orders of
      // magnitude bigger than that.

      const auto postResizeRectRaw = GetScreenBounds();
      const auto postResizeRect = postResizeRectRaw / GetDesktopToDeviceScale();
      const bool succeeded = postResizeRect.WithinEpsilonOf(rectAsFloat, 0.01);

      if (succeeded) {
        MOZ_LOG(sBaseWidgetLog, LogLevel::Debug,
                ("resized to: " MOZ_FORMAT_RECT("%f"),
                 MOZ_SPLAT_RECT(rectAsFloat)));
      } else {
        MOZ_LOG(sBaseWidgetLog, LogLevel::Warning,
                ("attempted to resize to: " MOZ_FORMAT_RECT("%f"),
                 MOZ_SPLAT_RECT(rectAsFloat)));
        MOZ_LOG(sBaseWidgetLog, LogLevel::Warning,
                ("... but ended up at: " MOZ_FORMAT_RECT("%f"),
                 MOZ_SPLAT_RECT(postResizeRect)));
      }

      MOZ_LOG(
          sBaseWidgetLog, LogLevel::Verbose,
          ("(... which, before DPI adjustment, is:" MOZ_FORMAT_RECT("%d") ")",
           MOZ_SPLAT_RECT(postResizeRectRaw)));
    }
  };

  if (aFullScreen) {
    if (!mSavedBounds) {
      mSavedBounds = Some(FullscreenSavedState());
    }
    // save current position
    mSavedBounds->windowRect = GetScreenBounds() / GetDesktopToDeviceScale();

    nsCOMPtr<nsIScreen> screen = GetWidgetScreen();
    if (!screen) {
      return;
    }

    // Move to fill the screen.
    doReposition(screen->GetRectDisplayPix());
    // Save off the new position. (This may differ from GetRectDisplayPix(), if
    // the OS was unhappy with it. See bug 1786226.)
    mSavedBounds->screenRect = GetScreenBounds() / GetDesktopToDeviceScale();
  } else {
    if (!mSavedBounds) {
      // This should never happen, at present, since we don't make windows
      // fullscreen at their creation time; but it's not logically impossible.
      MOZ_ASSERT(false, "fullscreen window did not have saved position");
      return;
    }

    // Figure out where to go from here.
    //
    // Fortunately, since we're currently fullscreen (and other code should be
    // handling _keeping_ us fullscreen even after display-layout changes),
    // there's an obvious choice for which display we should attach to; all we
    // need to determine is where on that display we should go.

    const DesktopRect currentWinRect =
        GetScreenBounds() / GetDesktopToDeviceScale();

    // Optimization: if where we are is where we were, then where we originally
    // came from is where we're going to go.
    if (currentWinRect == DesktopRect(mSavedBounds->screenRect)) {
      MOZ_LOG(sBaseWidgetLog, LogLevel::Debug,
              ("no location change detected; returning to saved location"));
      doReposition(mSavedBounds->windowRect);
      return;
    }

    /*
      General case: figure out where we're going to go by dividing where we are
      by where we were, and then multiplying by where we originally came from.

      Less abstrusely: resize so that we occupy the same proportional position
      on our current display after leaving fullscreen as we occupied on our
      previous display before entering fullscreen.

      (N.B.: We do not clamp. If we were only partially on the old display,
      we'll be only partially on the new one, too.)
    */

    MOZ_LOG(sBaseWidgetLog, LogLevel::Debug,
            ("location change detected; computing new destination"));

    // splat: convert an arbitrary Rect into a tuple, for syntactic convenience.
    const auto splat = [](auto rect) {
      return std::tuple(rect.X(), rect.Y(), rect.Width(), rect.Height());
    };

    // remap: find the unique affine mapping which transforms `src` to `dst`,
    // and apply it to `val`.
    using Range = std::pair<float, float>;
    const auto remap = [](Range dst, Range src, float val) {
      // linear interpolation and its inverse: lerp(a, b, invlerp(a, b, t)) == t
      const auto lerp = [](float lo, float hi, float t) {
        return lo + t * (hi - lo);
      };
      const auto invlerp = [](float lo, float hi, float mid) {
        return (mid - lo) / (hi - lo);
      };

      const auto [dst_a, dst_b] = dst;
      const auto [src_a, src_b] = src;
      return lerp(dst_a, dst_b, invlerp(src_a, src_b, val));
    };

    // original position
    const auto [px, py, pw, ph] = splat(mSavedBounds->windowRect);
    // source desktop rect
    const auto [sx, sy, sw, sh] = splat(mSavedBounds->screenRect);
    // target desktop rect
    const auto [tx, ty, tw, th] = splat(currentWinRect);

    const float nx = remap({tx, tx + tw}, {sx, sx + sw}, px);
    const float ny = remap({ty, ty + th}, {sy, sy + sh}, py);
    const float nw = remap({0, tw}, {0, sw}, pw);
    const float nh = remap({0, th}, {0, sh}, ph);

    doReposition(DesktopRect{nx, ny, nw, nh});
  }

#undef MOZ_SPLAT_RECT
#undef MOZ_FORMAT_RECT
}

nsresult nsBaseWidget::MakeFullScreen(bool aFullScreen) {
  InfallibleMakeFullScreen(aFullScreen);
  return NS_OK;
}

nsBaseWidget::AutoLayerManagerSetup::AutoLayerManagerSetup(
    nsBaseWidget* aWidget, gfxContext* aTarget, BufferMode aDoubleBuffering)
    : mWidget(aWidget) {
  WindowRenderer* renderer = mWidget->GetWindowRenderer();
  if (renderer->AsFallback()) {
    mRenderer = renderer->AsFallback();
    mRenderer->SetTarget(aTarget, aDoubleBuffering);
  }
}

nsBaseWidget::AutoLayerManagerSetup::~AutoLayerManagerSetup() {
  if (mRenderer) {
    mRenderer->SetTarget(nullptr, mozilla::layers::BufferMode::BUFFER_NONE);
  }
}

bool nsBaseWidget::IsSmallPopup() const {
  return mWindowType == WindowType::Popup && mPopupType != PopupType::Panel;
}

bool nsBaseWidget::ComputeShouldAccelerate() {
  return gfx::gfxConfig::IsEnabled(gfx::Feature::HW_COMPOSITING) &&
         (WidgetTypeSupportsAcceleration() ||
          StaticPrefs::gfx_webrender_unaccelerated_widget_force());
}

bool nsBaseWidget::UseAPZ() {
  return (gfxPlatform::AsyncPanZoomEnabled() &&
          (mWindowType == WindowType::TopLevel ||
           mWindowType == WindowType::Child ||
           ((mWindowType == WindowType::Popup ||
             mWindowType == WindowType::Dialog) &&
            HasRemoteContent() && StaticPrefs::apz_popups_enabled())));
}

void nsBaseWidget::CreateCompositor() {
  LayoutDeviceIntRect rect = GetBounds();
  CreateCompositor(rect.Width(), rect.Height());
}

void nsIWidget::PauseOrResumeCompositor(bool aPause) {
  auto* renderer = GetRemoteRenderer();
  if (!renderer) {
    return;
  }
  if (aPause) {
    renderer->SendPause();
  } else {
    renderer->SendResume();
  }
}

already_AddRefed<GeckoContentController>
nsBaseWidget::CreateRootContentController() {
  RefPtr<GeckoContentController> controller =
      new ChromeProcessController(this, mAPZEventState, mAPZC);
  return controller.forget();
}

void nsBaseWidget::ConfigureAPZCTreeManager() {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mAPZC);

  mAPZC->SetDPI(GetDPI());

  if (StaticPrefs::apz_keyboard_enabled_AtStartup()) {
    KeyboardMap map = RootWindowGlobalKeyListener::CollectKeyboardShortcuts();
    mAPZC->SetKeyboardMap(map);
  }

  ContentReceivedInputBlockCallback callback(
      [treeManager = RefPtr{mAPZC.get()}](uint64_t aInputBlockId,
                                          bool aPreventDefault) {
        MOZ_ASSERT(NS_IsMainThread());
        treeManager->ContentReceivedInputBlock(aInputBlockId, aPreventDefault);
      });
  mAPZEventState = new APZEventState(this, std::move(callback));

  mRootContentController = CreateRootContentController();
  if (mRootContentController) {
    mCompositorSession->SetContentController(mRootContentController);
  }

  // When APZ is enabled, we can actually enable raw touch events because we
  // have code that can deal with them properly. If APZ is not enabled, this
  // function doesn't get called.
  if (StaticPrefs::dom_w3c_touch_events_enabled()) {
    RegisterTouchWindow();
  }
}

void nsBaseWidget::ConfigureAPZControllerThread() {
  // By default the controller thread is the main thread.
  APZThreadUtils::SetControllerThread(NS_GetCurrentThread());
}

void nsBaseWidget::SetConfirmedTargetAPZC(
    uint64_t aInputBlockId,
    const nsTArray<ScrollableLayerGuid>& aTargets) const {
  mAPZC->SetTargetAPZC(aInputBlockId, aTargets);
}

void nsBaseWidget::UpdateZoomConstraints(
    const uint32_t& aPresShellId, const ScrollableLayerGuid::ViewID& aViewId,
    const Maybe<ZoomConstraints>& aConstraints) {
  if (!mCompositorSession || !mAPZC) {
    if (mInitialZoomConstraints) {
      MOZ_ASSERT(mInitialZoomConstraints->mPresShellID == aPresShellId);
      MOZ_ASSERT(mInitialZoomConstraints->mViewID == aViewId);
      if (!aConstraints) {
        mInitialZoomConstraints.reset();
      }
    }

    if (aConstraints) {
      // We have some constraints, but the compositor and APZC aren't created
      // yet. Save these so we can use them later.
      mInitialZoomConstraints = Some(
          InitialZoomConstraints(aPresShellId, aViewId, aConstraints.ref()));
    }
    return;
  }
  LayersId layersId = mCompositorSession->RootLayerTreeId();
  mAPZC->UpdateZoomConstraints(
      ScrollableLayerGuid(layersId, aPresShellId, aViewId), aConstraints);
}

bool nsBaseWidget::AsyncPanZoomEnabled() const { return !!mAPZC; }

nsEventStatus nsBaseWidget::ProcessUntransformedAPZEvent(
    WidgetInputEvent* aEvent, const APZEventResult& aApzResult) {
  MOZ_ASSERT(NS_IsMainThread());
  ScrollableLayerGuid targetGuid = aApzResult.mTargetGuid;
  uint64_t inputBlockId = aApzResult.mInputBlockId;
  InputAPZContext context(aApzResult.mTargetGuid, inputBlockId,
                          aApzResult.GetStatus());

  // Make a copy of the original event for the APZCCallbackHelper helpers that
  // we call later, because the event passed to DispatchEvent can get mutated in
  // ways that we don't want (i.e. touch points can get stripped out).
  nsEventStatus status;
  UniquePtr<WidgetEvent> original(aEvent->Duplicate());
  DispatchEvent(aEvent, status);

  if (mAPZC && !InputAPZContext::WasRoutedToChildProcess() &&
      !InputAPZContext::WasDropped() && inputBlockId) {
    // EventStateManager did not route the event into the child process and
    // the event was dispatched in the parent process.
    // It's safe to communicate to APZ that the event has been processed.
    // Note that here aGuid.mLayersId might be different from
    // mCompositorSession->RootLayerTreeId() because the event might have gotten
    // hit-tested by APZ to be targeted at a child process, but a parent process
    // event listener called preventDefault on it. In that case aGuid.mLayersId
    // would still be the layers id for the child process, but the event would
    // not have actually gotten routed to the child process. The main-thread
    // hit-test result therefore needs to use the parent process layers id.
    LayersId rootLayersId = mCompositorSession->RootLayerTreeId();

    RefPtr<DisplayportSetListener> postLayerization;
    if (WidgetTouchEvent* touchEvent = aEvent->AsTouchEvent()) {
      nsTArray<TouchBehaviorFlags> allowedTouchBehaviors;
      if (touchEvent->mMessage == eTouchStart) {
        auto& originalEvent = *original->AsTouchEvent();
        MOZ_ASSERT(NS_IsMainThread());
        allowedTouchBehaviors = TouchActionHelper::GetAllowedTouchBehavior(
            this, GetDocument(), originalEvent);
        if (!allowedTouchBehaviors.IsEmpty()) {
          mAPZC->SetAllowedTouchBehavior(inputBlockId, allowedTouchBehaviors);
        }
        postLayerization = APZCCallbackHelper::SendSetTargetAPZCNotification(
            this, GetDocument(), originalEvent, rootLayersId, inputBlockId);
      }
      mAPZEventState->ProcessTouchEvent(*touchEvent, targetGuid, inputBlockId,
                                        aApzResult.GetStatus(), status,
                                        std::move(allowedTouchBehaviors));
    } else if (WidgetWheelEvent* wheelEvent = aEvent->AsWheelEvent()) {
      MOZ_ASSERT(wheelEvent->mFlags.mHandledByAPZ);
      postLayerization = APZCCallbackHelper::SendSetTargetAPZCNotification(
          this, GetDocument(), *original->AsWheelEvent(), rootLayersId,
          inputBlockId);
      if (wheelEvent->mCanTriggerSwipe) {
        ReportSwipeStarted(inputBlockId, wheelEvent->TriggersSwipe());
      }
      mAPZEventState->ProcessWheelEvent(*wheelEvent, inputBlockId);
    } else if (WidgetMouseEvent* mouseEvent = aEvent->AsMouseEvent()) {
      MOZ_ASSERT(mouseEvent->mFlags.mHandledByAPZ);
      postLayerization = APZCCallbackHelper::SendSetTargetAPZCNotification(
          this, GetDocument(), *original->AsMouseEvent(), rootLayersId,
          inputBlockId);
      mAPZEventState->ProcessMouseEvent(*mouseEvent, inputBlockId);
    }
    if (postLayerization) {
      postLayerization->Register();
    }
  }

  return status;
}

template <class InputType, class EventType>
class DispatchEventOnMainThread : public Runnable {
 public:
  DispatchEventOnMainThread(const InputType& aInput, nsBaseWidget* aWidget,
                            const APZEventResult& aAPZResult)
      : mozilla::Runnable("DispatchEventOnMainThread"),
        mInput(aInput),
        mWidget(aWidget),
        mAPZResult(aAPZResult) {}

  NS_IMETHOD Run() override {
    EventType event = mInput.ToWidgetEvent(mWidget);
    mWidget->ProcessUntransformedAPZEvent(&event, mAPZResult);
    return NS_OK;
  }

 private:
  InputType mInput;
  nsBaseWidget* mWidget;
  APZEventResult mAPZResult;
};

template <class InputType, class EventType>
class DispatchInputOnControllerThread : public Runnable {
 public:
  DispatchInputOnControllerThread(const EventType& aEvent,
                                  IAPZCTreeManager* aAPZC,
                                  nsBaseWidget* aWidget)
      : mozilla::Runnable("DispatchInputOnControllerThread"),
        mMainMessageLoop(MessageLoop::current()),
        mInput(aEvent),
        mAPZC(aAPZC),
        mWidget(aWidget) {}

  NS_IMETHOD Run() override {
    APZEventResult result = mAPZC->InputBridge()->ReceiveInputEvent(mInput);
    if (result.GetStatus() == nsEventStatus_eConsumeNoDefault) {
      return NS_OK;
    }
    RefPtr<Runnable> r = new DispatchEventOnMainThread<InputType, EventType>(
        mInput, mWidget, result);
    mMainMessageLoop->PostTask(r.forget());
    return NS_OK;
  }

 private:
  MessageLoop* mMainMessageLoop;
  InputType mInput;
  RefPtr<IAPZCTreeManager> mAPZC;
  nsBaseWidget* mWidget;
};

void nsBaseWidget::DispatchTouchInput(MultiTouchInput& aInput,
                                      uint16_t aInputSource) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aInputSource ==
                 mozilla::dom::MouseEvent_Binding::MOZ_SOURCE_TOUCH ||
             aInputSource == mozilla::dom::MouseEvent_Binding::MOZ_SOURCE_PEN);
  if (mAPZC) {
    MOZ_ASSERT(APZThreadUtils::IsControllerThread());

    APZEventResult result = mAPZC->InputBridge()->ReceiveInputEvent(aInput);
    if (result.GetStatus() == nsEventStatus_eConsumeNoDefault) {
      return;
    }

    WidgetTouchEvent event = aInput.ToWidgetEvent(this, aInputSource);
    ProcessUntransformedAPZEvent(&event, result);
  } else {
    WidgetTouchEvent event = aInput.ToWidgetEvent(this, aInputSource);

    nsEventStatus status;
    DispatchEvent(&event, status);
  }
}

void nsBaseWidget::DispatchPanGestureInput(PanGestureInput& aInput) {
  MOZ_ASSERT(NS_IsMainThread());
  if (mAPZC) {
    MOZ_ASSERT(APZThreadUtils::IsControllerThread());

    APZEventResult result = mAPZC->InputBridge()->ReceiveInputEvent(aInput);
    if (result.GetStatus() == nsEventStatus_eConsumeNoDefault) {
      return;
    }

    WidgetWheelEvent event = aInput.ToWidgetEvent(this);
    ProcessUntransformedAPZEvent(&event, result);
  } else {
    WidgetWheelEvent event = aInput.ToWidgetEvent(this);
    nsEventStatus status;
    DispatchEvent(&event, status);
  }
}

void nsBaseWidget::DispatchPinchGestureInput(PinchGestureInput& aInput) {
  MOZ_ASSERT(NS_IsMainThread());
  if (mAPZC) {
    MOZ_ASSERT(APZThreadUtils::IsControllerThread());
    APZEventResult result = mAPZC->InputBridge()->ReceiveInputEvent(aInput);

    if (result.GetStatus() == nsEventStatus_eConsumeNoDefault) {
      return;
    }
    WidgetWheelEvent event = aInput.ToWidgetEvent(this);
    ProcessUntransformedAPZEvent(&event, result);
  } else {
    WidgetWheelEvent event = aInput.ToWidgetEvent(this);
    nsEventStatus status;
    DispatchEvent(&event, status);
  }
}

nsIWidget::ContentAndAPZEventStatus nsBaseWidget::DispatchInputEvent(
    WidgetInputEvent* aEvent) {
  nsIWidget::ContentAndAPZEventStatus status;
  MOZ_ASSERT(NS_IsMainThread());
  if (mAPZC) {
    if (APZThreadUtils::IsControllerThread()) {
      APZEventResult result = mAPZC->InputBridge()->ReceiveInputEvent(*aEvent);
      status.mApzStatus = result.GetStatus();
      if (result.GetStatus() == nsEventStatus_eConsumeNoDefault) {
        return status;
      }
      status.mContentStatus = ProcessUntransformedAPZEvent(aEvent, result);
      return status;
    }
    if (WidgetWheelEvent* wheelEvent = aEvent->AsWheelEvent()) {
      RefPtr<Runnable> r =
          new DispatchInputOnControllerThread<ScrollWheelInput,
                                              WidgetWheelEvent>(*wheelEvent,
                                                                mAPZC, this);
      APZThreadUtils::RunOnControllerThread(std::move(r));
      status.mContentStatus = nsEventStatus_eConsumeDoDefault;
      return status;
    }
    if (WidgetMouseEvent* mouseEvent = aEvent->AsMouseEvent()) {
      RefPtr<Runnable> r =
          new DispatchInputOnControllerThread<MouseInput, WidgetMouseEvent>(
              *mouseEvent, mAPZC, this);
      APZThreadUtils::RunOnControllerThread(std::move(r));
      status.mContentStatus = nsEventStatus_eConsumeDoDefault;
      return status;
    }
    if (WidgetTouchEvent* touchEvent = aEvent->AsTouchEvent()) {
      RefPtr<Runnable> r =
          new DispatchInputOnControllerThread<MultiTouchInput,
                                              WidgetTouchEvent>(*touchEvent,
                                                                mAPZC, this);
      APZThreadUtils::RunOnControllerThread(std::move(r));
      status.mContentStatus = nsEventStatus_eConsumeDoDefault;
      return status;
    }
    // Allow dispatching keyboard events on Gecko thread.
    MOZ_ASSERT(aEvent->AsKeyboardEvent());
  }

  DispatchEvent(aEvent, status.mContentStatus);
  return status;
}

void nsBaseWidget::DispatchEventToAPZOnly(mozilla::WidgetInputEvent* aEvent) {
  MOZ_ASSERT(NS_IsMainThread());
  if (mAPZC) {
    MOZ_ASSERT(APZThreadUtils::IsControllerThread());
    mAPZC->InputBridge()->ReceiveInputEvent(*aEvent);
  }
}

bool nsBaseWidget::DispatchWindowEvent(WidgetGUIEvent& event) {
  nsEventStatus status;
  DispatchEvent(&event, status);
  return ConvertStatus(status);
}

Document* nsBaseWidget::GetDocument() const {
  if (mWidgetListener) {
    if (PresShell* presShell = mWidgetListener->GetPresShell()) {
      return presShell->GetDocument();
    }
  }
  return nullptr;
}

void nsBaseWidget::CreateCompositorVsyncDispatcher() {
  // Parent directly listens to the vsync source whereas
  // child process communicate via IPC
  // Should be called AFTER gfxPlatform is initialized
  if (XRE_IsParentProcess()) {
    if (!mCompositorVsyncDispatcherLock) {
      mCompositorVsyncDispatcherLock =
          MakeUnique<Mutex>("mCompositorVsyncDispatcherLock");
    }
    MutexAutoLock lock(*mCompositorVsyncDispatcherLock.get());
    if (!mCompositorVsyncDispatcher) {
      RefPtr<VsyncDispatcher> vsyncDispatcher =
          gfxPlatform::GetPlatform()->GetGlobalVsyncDispatcher();
      mCompositorVsyncDispatcher =
          new CompositorVsyncDispatcher(std::move(vsyncDispatcher));
    }
  }
}

already_AddRefed<CompositorVsyncDispatcher>
nsBaseWidget::GetCompositorVsyncDispatcher() {
  MOZ_ASSERT(mCompositorVsyncDispatcherLock.get());

  MutexAutoLock lock(*mCompositorVsyncDispatcherLock.get());
  RefPtr<CompositorVsyncDispatcher> dispatcher = mCompositorVsyncDispatcher;
  return dispatcher.forget();
}

already_AddRefed<WebRenderLayerManager> nsBaseWidget::CreateCompositorSession(
    int aWidth, int aHeight, CompositorOptions* aOptionsOut) {
  MOZ_ASSERT(aOptionsOut);

  do {
    CreateCompositorVsyncDispatcher();

    gfx::GPUProcessManager* gpu = gfx::GPUProcessManager::Get();
    // Make sure GPU process is ready for use.
    // If it failed to connect to GPU process, GPU process usage is disabled in
    // EnsureGPUReady(). It could update gfxVars and gfxConfigs.
    nsresult rv = gpu->EnsureGPUReady();
    if (NS_WARN_IF(rv == NS_ERROR_ILLEGAL_DURING_SHUTDOWN)) {
      return nullptr;
    }

    // If widget type does not supports acceleration, we may be allowed to use
    // software WebRender instead.
    bool supportsAcceleration = WidgetTypeSupportsAcceleration();
    bool enableSWWR = true;
    if (supportsAcceleration ||
        StaticPrefs::gfx_webrender_unaccelerated_widget_force()) {
      enableSWWR = gfx::gfxVars::UseSoftwareWebRender();
    }
    bool enableAPZ = UseAPZ();
    CompositorOptions options(enableAPZ, enableSWWR);

#ifdef XP_WIN
    if (supportsAcceleration) {
      options.SetAllowSoftwareWebRenderD3D11(
          gfx::gfxVars::AllowSoftwareWebRenderD3D11());
    }
    if (mNeedFastSnaphot) {
      options.SetNeedFastSnaphot(true);
    }
#elif defined(MOZ_WIDGET_ANDROID)
    MOZ_ASSERT(supportsAcceleration);
    options.SetAllowSoftwareWebRenderOGL(
        gfx::gfxVars::AllowSoftwareWebRenderOGL());
#elif defined(MOZ_WIDGET_GTK)
    if (supportsAcceleration) {
      options.SetAllowSoftwareWebRenderOGL(
          gfx::gfxVars::AllowSoftwareWebRenderOGL());
    }
#endif

#ifdef MOZ_WIDGET_ANDROID
    // Unconditionally set the compositor as initially paused, as we have not
    // yet had a chance to send the compositor surface to the GPU process. We
    // will do so shortly once we have returned to nsWindow::CreateLayerManager,
    // where we will also resume the compositor if required.
    options.SetInitiallyPaused(true);
#else
    options.SetInitiallyPaused(CompositorInitiallyPaused());
#endif

    RefPtr<WebRenderLayerManager> lm = new WebRenderLayerManager(this);

    uint64_t innerWindowId = 0;
    if (Document* doc = GetDocument()) {
      innerWindowId = doc->InnerWindowID();
    }

    bool retry = false;
    mCompositorSession = gpu->CreateTopLevelCompositor(
        this, lm, GetDefaultScale(), options, UseExternalCompositingSurface(),
        gfx::IntSize(aWidth, aHeight), innerWindowId, &retry);

    if (mCompositorSession) {
      TextureFactoryIdentifier textureFactoryIdentifier;
      nsCString error;
      lm->Initialize(mCompositorSession->GetCompositorBridgeChild(),
                     wr::AsPipelineId(mCompositorSession->RootLayerTreeId()),
                     &textureFactoryIdentifier, error);
      if (textureFactoryIdentifier.mParentBackend != LayersBackend::LAYERS_WR) {
        retry = true;
        DestroyCompositor();
        // gfxVars::UseDoubleBufferingWithCompositor() is also disabled.
        gfx::GPUProcessManager::Get()->DisableWebRender(
            wr::WebRenderError::INITIALIZE, error);
      }
    }

    // We need to retry in a loop because the act of failing to create the
    // compositor can change our state (e.g. disable WebRender).
    if (mCompositorSession || !retry) {
      *aOptionsOut = options;
      return lm.forget();
    }
  } while (true);
}

void nsBaseWidget::CreateCompositor(int aWidth, int aHeight) {
  // This makes sure that gfxPlatforms gets initialized if it hasn't by now.
  gfxPlatform::GetPlatform();

  MOZ_ASSERT(gfxPlatform::UsesOffMainThreadCompositing(),
             "This function assumes OMTC");

  MOZ_ASSERT(!mCompositorSession && !mCompositorBridgeChild,
             "Should have properly cleaned up the previous PCompositor pair "
             "beforehand");

  if (mCompositorBridgeChild) {
    mCompositorBridgeChild->Destroy();
  }

  // Recreating this is tricky, as we may still have an old and we need
  // to make sure it's properly destroyed by calling DestroyCompositor!

  // If we've already received a shutdown notification, don't try
  // create a new compositor.
  if (!mShutdownObserver) {
    return;
  }

  // The controller thread must be configured before the compositor
  // session is created, so that the input bridge runs on the right
  // thread.
  ConfigureAPZControllerThread();

  CompositorOptions options;
  RefPtr<WebRenderLayerManager> lm =
      CreateCompositorSession(aWidth, aHeight, &options);
  if (!lm) {
    return;
  }

  MOZ_ASSERT(mCompositorSession);
  mCompositorBridgeChild = mCompositorSession->GetCompositorBridgeChild();
  SetCompositorWidgetDelegate(
      mCompositorSession->GetCompositorWidgetDelegate());

  if (options.UseAPZ()) {
    mAPZC = mCompositorSession->GetAPZCTreeManager();
    ConfigureAPZCTreeManager();
  } else {
    mAPZC = nullptr;
  }

  if (mInitialZoomConstraints) {
    UpdateZoomConstraints(mInitialZoomConstraints->mPresShellID,
                          mInitialZoomConstraints->mViewID,
                          Some(mInitialZoomConstraints->mConstraints));
    mInitialZoomConstraints.reset();
  }

  TextureFactoryIdentifier textureFactoryIdentifier =
      lm->GetTextureFactoryIdentifier();
  MOZ_ASSERT(textureFactoryIdentifier.mParentBackend ==
             LayersBackend::LAYERS_WR);
  ImageBridgeChild::IdentifyCompositorTextureHost(textureFactoryIdentifier);
  gfx::VRManagerChild::IdentifyTextureHost(textureFactoryIdentifier);

  WindowUsesOMTC();

  mWindowRenderer = std::move(lm);

  // Only track compositors for top-level windows, since other window types
  // may use the basic compositor.  Except on the OS X - see bug 1306383
#if defined(XP_MACOSX)
  bool getCompositorFromThisWindow = true;
#else
  bool getCompositorFromThisWindow = mWindowType == WindowType::TopLevel;
#endif

  if (getCompositorFromThisWindow) {
    gfxPlatform::GetPlatform()->NotifyCompositorCreated(
        mWindowRenderer->GetCompositorBackendType());
  }
}

void nsBaseWidget::NotifyCompositorSessionLost(CompositorSession* aSession) {
  MOZ_ASSERT(aSession == mCompositorSession);
  DestroyLayerManager();
}

bool nsBaseWidget::ShouldUseOffMainThreadCompositing() {
  return gfxPlatform::UsesOffMainThreadCompositing();
}

WindowRenderer* nsBaseWidget::GetWindowRenderer() {
  if (!mWindowRenderer) {
    if (!mShutdownObserver) {
      // We are shutting down, do not try to re-create a LayerManager
      return nullptr;
    }
    // Try to use an async compositor first, if possible
    if (ShouldUseOffMainThreadCompositing()) {
      CreateCompositor();
    }

    if (!mWindowRenderer) {
      mWindowRenderer = CreateFallbackRenderer();
    }
  }
  return mWindowRenderer;
}

WindowRenderer* nsBaseWidget::CreateFallbackRenderer() {
  return new FallbackRenderer;
}

CompositorBridgeChild* nsBaseWidget::GetRemoteRenderer() {
  return mCompositorBridgeChild;
}

void nsBaseWidget::ClearCachedWebrenderResources() {
  if (!mWindowRenderer || !mWindowRenderer->AsWebRender()) {
    return;
  }
  mWindowRenderer->AsWebRender()->ClearCachedResources();
}

void nsBaseWidget::ClearWebrenderAnimationResources() {
  if (!mWindowRenderer || !mWindowRenderer->AsWebRender()) {
    return;
  }
  mWindowRenderer->AsWebRender()->ClearAnimationResources();
}

bool nsBaseWidget::SetNeedFastSnaphot() {
  MOZ_ASSERT(XRE_IsParentProcess());
  MOZ_ASSERT(!mCompositorSession);

  if (!XRE_IsParentProcess() || mCompositorSession) {
    return false;
  }

  mNeedFastSnaphot = true;
  return true;
}

already_AddRefed<gfx::DrawTarget> nsBaseWidget::StartRemoteDrawing() {
  return nullptr;
}

uint32_t nsBaseWidget::GetGLFrameBufferFormat() { return LOCAL_GL_RGBA; }

//-------------------------------------------------------------------------
//
// Destroy the window
//
//-------------------------------------------------------------------------
void nsBaseWidget::OnDestroy() {
  if (mTextEventDispatcher) {
    mTextEventDispatcher->OnDestroyWidget();
    // Don't release it until this widget actually released because after this
    // is called, TextEventDispatcher() may create it again.
  }

  // If this widget is being destroyed, let the APZ code know to drop references
  // to this widget. Callers of this function all should be holding a deathgrip
  // on this widget already.
  ReleaseContentController();
}

void nsBaseWidget::MoveClient(const DesktopPoint& aOffset) {
  LayoutDeviceIntPoint clientOffset(GetClientOffset());

  // GetClientOffset returns device pixels; scale back to desktop pixels
  // if that's what this widget uses for the Move/Resize APIs
  if (BoundsUseDesktopPixels()) {
    DesktopPoint desktopOffset = clientOffset / GetDesktopToDeviceScale();
    Move(aOffset.x - desktopOffset.x, aOffset.y - desktopOffset.y);
  } else {
    LayoutDevicePoint layoutOffset = aOffset * GetDesktopToDeviceScale();
    Move(layoutOffset.x - LayoutDeviceCoord(clientOffset.x),
         layoutOffset.y - LayoutDeviceCoord(clientOffset.y));
  }
}

void nsBaseWidget::ResizeClient(const DesktopSize& aSize, bool aRepaint) {
  NS_ASSERTION((aSize.width >= 0), "Negative width passed to ResizeClient");
  NS_ASSERTION((aSize.height >= 0), "Negative height passed to ResizeClient");

  LayoutDeviceIntRect clientBounds = GetClientBounds();

  // GetClientBounds and mBounds are device pixels; scale back to desktop pixels
  // if that's what this widget uses for the Move/Resize APIs
  if (BoundsUseDesktopPixels()) {
    DesktopSize desktopDelta =
        (LayoutDeviceIntSize(mBounds.Width(), mBounds.Height()) -
         clientBounds.Size()) /
        GetDesktopToDeviceScale();
    Resize(aSize.width + desktopDelta.width, aSize.height + desktopDelta.height,
           aRepaint);
  } else {
    LayoutDeviceSize layoutSize = aSize * GetDesktopToDeviceScale();
    Resize(mBounds.Width() + (layoutSize.width - clientBounds.Width()),
           mBounds.Height() + (layoutSize.height - clientBounds.Height()),
           aRepaint);
  }
}

void nsBaseWidget::ResizeClient(const DesktopRect& aRect, bool aRepaint) {
  NS_ASSERTION((aRect.Width() >= 0), "Negative width passed to ResizeClient");
  NS_ASSERTION((aRect.Height() >= 0), "Negative height passed to ResizeClient");

  LayoutDeviceIntRect clientBounds = GetClientBounds();
  LayoutDeviceIntPoint clientOffset = GetClientOffset();
  DesktopToLayoutDeviceScale scale = GetDesktopToDeviceScale();

  if (BoundsUseDesktopPixels()) {
    DesktopPoint desktopOffset = clientOffset / scale;
    DesktopSize desktopDelta =
        (LayoutDeviceIntSize(mBounds.Width(), mBounds.Height()) -
         clientBounds.Size()) /
        scale;
    Resize(aRect.X() - desktopOffset.x, aRect.Y() - desktopOffset.y,
           aRect.Width() + desktopDelta.width,
           aRect.Height() + desktopDelta.height, aRepaint);
  } else {
    LayoutDeviceRect layoutRect = aRect * scale;
    Resize(layoutRect.X() - clientOffset.x, layoutRect.Y() - clientOffset.y,
           layoutRect.Width() + mBounds.Width() - clientBounds.Width(),
           layoutRect.Height() + mBounds.Height() - clientBounds.Height(),
           aRepaint);
  }
}

//-------------------------------------------------------------------------
//
// Bounds
//
//-------------------------------------------------------------------------

/**
 * If the implementation of nsWindow supports borders this method MUST be
 * overridden
 *
 **/
LayoutDeviceIntRect nsBaseWidget::GetClientBounds() { return GetBounds(); }

/**
 * If the implementation of nsWindow supports borders this method MUST be
 * overridden
 *
 **/
LayoutDeviceIntRect nsBaseWidget::GetBounds() { return mBounds; }

/**
 * If the implementation of nsWindow uses a local coordinate system within the
 *window, this method must be overridden
 *
 **/
LayoutDeviceIntRect nsBaseWidget::GetScreenBounds() { return GetBounds(); }

nsresult nsBaseWidget::GetRestoredBounds(LayoutDeviceIntRect& aRect) {
  if (SizeMode() != nsSizeMode_Normal) {
    return NS_ERROR_FAILURE;
  }
  aRect = GetScreenBounds();
  return NS_OK;
}

LayoutDeviceIntPoint nsBaseWidget::GetClientOffset() {
  return LayoutDeviceIntPoint(0, 0);
}

nsresult nsBaseWidget::SetNonClientMargins(const LayoutDeviceIntMargin&) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

void nsBaseWidget::SetResizeMargin(LayoutDeviceIntCoord aResizeMargin) {}

uint32_t nsBaseWidget::GetMaxTouchPoints() const { return 0; }

bool nsBaseWidget::HasPendingInputEvent() { return false; }

bool nsBaseWidget::ShowsResizeIndicator(LayoutDeviceIntRect* aResizerRect) {
  return false;
}

/**
 * Modifies aFile to point at an icon file with the given name and suffix.  The
 * suffix may correspond to a file extension with leading '.' if appropriate.
 * Returns true if the icon file exists and can be read.
 */
static bool ResolveIconNameHelper(nsIFile* aFile, const nsAString& aIconName,
                                  const nsAString& aIconSuffix) {
  aFile->Append(u"icons"_ns);
  aFile->Append(u"default"_ns);
  aFile->Append(aIconName + aIconSuffix);

  bool readable;
  return NS_SUCCEEDED(aFile->IsReadable(&readable)) && readable;
}

/**
 * Resolve the given icon name into a local file object.  This method is
 * intended to be called by subclasses of nsBaseWidget.  aIconSuffix is a
 * platform specific icon file suffix (e.g., ".ico" under Win32).
 *
 * If no file is found matching the given parameters, then null is returned.
 */
void nsBaseWidget::ResolveIconName(const nsAString& aIconName,
                                   const nsAString& aIconSuffix,
                                   nsIFile** aResult) {
  *aResult = nullptr;

  nsCOMPtr<nsIProperties> dirSvc =
      do_GetService(NS_DIRECTORY_SERVICE_CONTRACTID);
  if (!dirSvc) return;

  // first check auxilary chrome directories

  nsCOMPtr<nsISimpleEnumerator> dirs;
  dirSvc->Get(NS_APP_CHROME_DIR_LIST, NS_GET_IID(nsISimpleEnumerator),
              getter_AddRefs(dirs));
  if (dirs) {
    bool hasMore;
    while (NS_SUCCEEDED(dirs->HasMoreElements(&hasMore)) && hasMore) {
      nsCOMPtr<nsISupports> element;
      dirs->GetNext(getter_AddRefs(element));
      if (!element) continue;
      nsCOMPtr<nsIFile> file = do_QueryInterface(element);
      if (!file) continue;
      if (ResolveIconNameHelper(file, aIconName, aIconSuffix)) {
        NS_ADDREF(*aResult = file);
        return;
      }
    }
  }

  // then check the main app chrome directory

  nsCOMPtr<nsIFile> file;
  dirSvc->Get(NS_APP_CHROME_DIR, NS_GET_IID(nsIFile), getter_AddRefs(file));
  if (file && ResolveIconNameHelper(file, aIconName, aIconSuffix))
    NS_ADDREF(*aResult = file);
}

void nsBaseWidget::SetSizeConstraints(const SizeConstraints& aConstraints) {
  mSizeConstraints = aConstraints;

  // Popups are constrained during layout, and we don't want to synchronously
  // paint from reflow, so bail out... This is not great, but it's no worse than
  // what we used to do.
  //
  // The right fix here is probably making constraint changes go through the
  // view manager and such.
  if (mWindowType == WindowType::Popup) {
    return;
  }

  // If the current size doesn't meet the new constraints, trigger a
  // resize to apply it. Note that, we don't want to invoke Resize if
  // the new constraints don't affect the current size, because Resize
  // implementation on some platforms may touch other geometry even if
  // the size don't need to change.
  LayoutDeviceIntSize curSize = mBounds.Size();
  LayoutDeviceIntSize clampedSize =
      Max(aConstraints.mMinSize, Min(aConstraints.mMaxSize, curSize));
  if (clampedSize != curSize) {
    gfx::Size size;
    if (BoundsUseDesktopPixels()) {
      DesktopSize desktopSize = clampedSize / GetDesktopToDeviceScale();
      size = desktopSize.ToUnknownSize();
    } else {
      size = gfx::Size(clampedSize.ToUnknownSize());
    }
    Resize(size.width, size.height, true);
  }
}

const widget::SizeConstraints nsBaseWidget::GetSizeConstraints() {
  return mSizeConstraints;
}

// static
nsIRollupListener* nsBaseWidget::GetActiveRollupListener() {
  // TODO: Simplify this.
  return nsXULPopupManager::GetInstance();
}

void nsBaseWidget::NotifyWindowDestroyed() {
  if (!mWidgetListener) return;

  nsCOMPtr<nsIAppWindow> window = mWidgetListener->GetAppWindow();
  nsCOMPtr<nsIBaseWindow> appWindow(do_QueryInterface(window));
  if (appWindow) {
    appWindow->Destroy();
  }
}

void nsBaseWidget::NotifyWindowMoved(int32_t aX, int32_t aY,
                                     ByMoveToRect aByMoveToRect) {
  if (mWidgetListener) {
    mWidgetListener->WindowMoved(this, aX, aY, aByMoveToRect);
  }

  if (mIMEHasFocus && IMENotificationRequestsRef().WantPositionChanged()) {
    NotifyIME(IMENotification(IMEMessage::NOTIFY_IME_OF_POSITION_CHANGE));
  }
}

void nsBaseWidget::NotifySizeMoveDone() {
  if (!mWidgetListener) {
    return;
  }
  if (PresShell* presShell = mWidgetListener->GetPresShell()) {
    presShell->WindowSizeMoveDone();
  }
}

void nsBaseWidget::NotifyThemeChanged(ThemeChangeKind aKind) {
  LookAndFeel::NotifyChangedAllWindows(aKind);
}

nsresult nsBaseWidget::NotifyIME(const IMENotification& aIMENotification) {
  if (mIMEHasQuit) {
    return NS_OK;
  }
  switch (aIMENotification.mMessage) {
    case REQUEST_TO_COMMIT_COMPOSITION:
    case REQUEST_TO_CANCEL_COMPOSITION:
      // We should send request to IME only when there is a TextEventDispatcher
      // instance (this means that this widget has dispatched at least one
      // composition event or keyboard event) and the it has composition.
      // Otherwise, there is nothing to do.
      // Note that if current input transaction is for native input events,
      // TextEventDispatcher::NotifyIME() will call
      // TextEventDispatcherListener::NotifyIME().
      if (mTextEventDispatcher && mTextEventDispatcher->IsComposing()) {
        return mTextEventDispatcher->NotifyIME(aIMENotification);
      }
      return NS_OK;
    default: {
      if (aIMENotification.mMessage == NOTIFY_IME_OF_FOCUS) {
        mIMEHasFocus = true;
      }
      EnsureTextEventDispatcher();
      // TextEventDispatcher::NotifyIME() will always call
      // TextEventDispatcherListener::NotifyIME().  I.e., even if current
      // input transaction is for synthesized events for automated tests,
      // notifications will be sent to native IME.
      nsresult rv = mTextEventDispatcher->NotifyIME(aIMENotification);
      if (aIMENotification.mMessage == NOTIFY_IME_OF_BLUR) {
        mIMEHasFocus = false;
      }
      return rv;
    }
  }
}

void nsBaseWidget::EnsureTextEventDispatcher() {
  if (mTextEventDispatcher) {
    return;
  }
  mTextEventDispatcher = new TextEventDispatcher(this);
}

nsIWidget::NativeIMEContext nsBaseWidget::GetNativeIMEContext() {
  if (mTextEventDispatcher && mTextEventDispatcher->GetPseudoIMEContext()) {
    // If we already have a TextEventDispatcher and it's working with
    // a TextInputProcessor, we need to return pseudo IME context since
    // TextCompositionArray::IndexOf(nsIWidget*) should return a composition
    // on the pseudo IME context in such case.
    NativeIMEContext pseudoIMEContext;
    pseudoIMEContext.InitWithRawNativeIMEContext(
        mTextEventDispatcher->GetPseudoIMEContext());
    return pseudoIMEContext;
  }
  return NativeIMEContext(this);
}

nsIWidget::TextEventDispatcher* nsBaseWidget::GetTextEventDispatcher() {
  EnsureTextEventDispatcher();
  return mTextEventDispatcher;
}

void* nsBaseWidget::GetPseudoIMEContext() {
  TextEventDispatcher* dispatcher = GetTextEventDispatcher();
  if (!dispatcher) {
    return nullptr;
  }
  return dispatcher->GetPseudoIMEContext();
}

TextEventDispatcherListener*
nsBaseWidget::GetNativeTextEventDispatcherListener() {
  // TODO: If all platforms supported use of TextEventDispatcher for handling
  //       native IME and keyboard events, this method should be removed since
  //       in such case, this is overridden by all the subclasses.
  return nullptr;
}

void nsBaseWidget::ZoomToRect(const uint32_t& aPresShellId,
                              const ScrollableLayerGuid::ViewID& aViewId,
                              const CSSRect& aRect, const uint32_t& aFlags) {
  if (!mCompositorSession || !mAPZC) {
    return;
  }
  LayersId layerId = mCompositorSession->RootLayerTreeId();
  mAPZC->ZoomToRect(ScrollableLayerGuid(layerId, aPresShellId, aViewId),
                    ZoomTarget{aRect}, aFlags);
}

#ifdef ACCESSIBILITY

a11y::LocalAccessible* nsBaseWidget::GetRootAccessible() {
  NS_ENSURE_TRUE(mWidgetListener, nullptr);

  PresShell* presShell = mWidgetListener->GetPresShell();
  NS_ENSURE_TRUE(presShell, nullptr);

  // If container is null then the presshell is not active. This often happens
  // when a preshell is being held onto for fastback.
  nsPresContext* presContext = presShell->GetPresContext();
  NS_ENSURE_TRUE(presContext->GetContainerWeak(), nullptr);

  // LocalAccessible creation might be not safe so use IsSafeToRunScript to
  // make sure it's not created at unsafe times.
  nsAccessibilityService* accService = GetOrCreateAccService();
  if (accService) {
    return accService->GetRootDocumentAccessible(
        presShell, nsContentUtils::IsSafeToRunScript());
  }

  return nullptr;
}

#endif  // ACCESSIBILITY

void nsBaseWidget::StartAsyncScrollbarDrag(
    const AsyncDragMetrics& aDragMetrics) {
  if (!AsyncPanZoomEnabled()) {
    return;
  }

  MOZ_ASSERT(XRE_IsParentProcess() && mCompositorSession);

  LayersId layersId = mCompositorSession->RootLayerTreeId();
  ScrollableLayerGuid guid(layersId, aDragMetrics.mPresShellId,
                           aDragMetrics.mViewId);

  mAPZC->StartScrollbarDrag(guid, aDragMetrics);
}

bool nsBaseWidget::StartAsyncAutoscroll(const ScreenPoint& aAnchorLocation,
                                        const ScrollableLayerGuid& aGuid) {
  MOZ_ASSERT(XRE_IsParentProcess() && AsyncPanZoomEnabled());

  return mAPZC->StartAutoscroll(aGuid, aAnchorLocation);
}

void nsBaseWidget::StopAsyncAutoscroll(const ScrollableLayerGuid& aGuid) {
  MOZ_ASSERT(XRE_IsParentProcess() && AsyncPanZoomEnabled());

  mAPZC->StopAutoscroll(aGuid);
}

LayersId nsBaseWidget::GetRootLayerTreeId() {
  return mCompositorSession ? mCompositorSession->RootLayerTreeId()
                            : LayersId{0};
}

already_AddRefed<widget::Screen> nsBaseWidget::GetWidgetScreen() {
  ScreenManager& screenManager = ScreenManager::GetSingleton();
  LayoutDeviceIntRect bounds = GetScreenBounds();
  DesktopIntRect deskBounds = RoundedToInt(bounds / GetDesktopToDeviceScale());
  return screenManager.ScreenForRect(deskBounds);
}

mozilla::DesktopToLayoutDeviceScale
nsBaseWidget::GetDesktopToDeviceScaleByScreen() {
  return (nsView::GetViewFor(this)->GetViewManager()->GetDeviceContext())
      ->GetDesktopToDeviceScale();
}

nsresult nsIWidget::SynthesizeNativeTouchTap(LayoutDeviceIntPoint aPoint,
                                             bool aLongTap,
                                             nsIObserver* aObserver) {
  AutoObserverNotifier notifier(aObserver, "touchtap");

  if (sPointerIdCounter > TOUCH_INJECT_MAX_POINTS) {
    sPointerIdCounter = 0;
  }
  int pointerId = sPointerIdCounter;
  sPointerIdCounter++;
  nsresult rv = SynthesizeNativeTouchPoint(pointerId, TOUCH_CONTACT, aPoint,
                                           1.0, 90, nullptr);
  if (NS_FAILED(rv)) {
    return rv;
  }

  if (!aLongTap) {
    return SynthesizeNativeTouchPoint(pointerId, TOUCH_REMOVE, aPoint, 0, 0,
                                      nullptr);
  }

  // initiate a long tap
  int elapse = Preferences::GetInt("ui.click_hold_context_menus.delay",
                                   TOUCH_INJECT_LONG_TAP_DEFAULT_MSEC);
  if (!mLongTapTimer) {
    mLongTapTimer = NS_NewTimer();
    if (!mLongTapTimer) {
      SynthesizeNativeTouchPoint(pointerId, TOUCH_CANCEL, aPoint, 0, 0,
                                 nullptr);
      return NS_ERROR_UNEXPECTED;
    }
    // Windows requires recuring events, so we set this to a smaller window
    // than the pref value.
    int timeout = elapse;
    if (timeout > TOUCH_INJECT_PUMP_TIMER_MSEC) {
      timeout = TOUCH_INJECT_PUMP_TIMER_MSEC;
    }
    mLongTapTimer->InitWithNamedFuncCallback(
        OnLongTapTimerCallback, this, timeout, nsITimer::TYPE_REPEATING_SLACK,
        "nsIWidget::SynthesizeNativeTouchTap");
  }

  // If we already have a long tap pending, cancel it. We only allow one long
  // tap to be active at a time.
  if (mLongTapTouchPoint) {
    SynthesizeNativeTouchPoint(mLongTapTouchPoint->mPointerId, TOUCH_CANCEL,
                               mLongTapTouchPoint->mPosition, 0, 0, nullptr);
  }

  mLongTapTouchPoint = MakeUnique<LongTapInfo>(
      pointerId, aPoint, TimeDuration::FromMilliseconds(elapse), aObserver);
  notifier.SkipNotification();  // we'll do it in the long-tap callback
  return NS_OK;
}

// static
void nsIWidget::OnLongTapTimerCallback(nsITimer* aTimer, void* aClosure) {
  auto* self = static_cast<nsIWidget*>(aClosure);

  if ((self->mLongTapTouchPoint->mStamp + self->mLongTapTouchPoint->mDuration) >
      TimeStamp::Now()) {
#ifdef XP_WIN
    // Windows needs us to keep pumping feedback to the digitizer, so update
    // the pointer id with the same position.
    self->SynthesizeNativeTouchPoint(
        self->mLongTapTouchPoint->mPointerId, TOUCH_CONTACT,
        self->mLongTapTouchPoint->mPosition, 1.0, 90, nullptr);
#endif
    return;
  }

  AutoObserverNotifier notifier(self->mLongTapTouchPoint->mObserver,
                                "touchtap");

  // finished, remove the touch point
  self->mLongTapTimer->Cancel();
  self->mLongTapTimer = nullptr;
  self->SynthesizeNativeTouchPoint(
      self->mLongTapTouchPoint->mPointerId, TOUCH_REMOVE,
      self->mLongTapTouchPoint->mPosition, 0, 0, nullptr);
  self->mLongTapTouchPoint = nullptr;
}

float nsIWidget::GetFallbackDPI() {
  RefPtr<const Screen> primaryScreen =
      ScreenManager::GetSingleton().GetPrimaryScreen();
  return primaryScreen->GetDPI();
}

CSSToLayoutDeviceScale nsIWidget::GetFallbackDefaultScale() {
  RefPtr<const Screen> s = ScreenManager::GetSingleton().GetPrimaryScreen();
  return s->GetCSSToLayoutDeviceScale(Screen::IncludeOSZoom::No);
}

nsresult nsIWidget::ClearNativeTouchSequence(nsIObserver* aObserver) {
  AutoObserverNotifier notifier(aObserver, "cleartouch");

  // XXX This is odd.  This is called by the constructor of nsIWidget.  However,
  //     at that point, nsIWidget::mLongTapTimer must be nullptr.  Therefore,
  //     this must do nothing at initializing the instance.
  if (!mLongTapTimer) {
    return NS_OK;
  }
  mLongTapTimer->Cancel();
  mLongTapTimer = nullptr;
  SynthesizeNativeTouchPoint(mLongTapTouchPoint->mPointerId, TOUCH_CANCEL,
                             mLongTapTouchPoint->mPosition, 0, 0, nullptr);
  mLongTapTouchPoint = nullptr;
  return NS_OK;
}

MultiTouchInput nsBaseWidget::UpdateSynthesizedTouchState(
    MultiTouchInput* aState, mozilla::TimeStamp aTimeStamp, uint32_t aPointerId,
    TouchPointerState aPointerState, LayoutDeviceIntPoint aPoint,
    double aPointerPressure, uint32_t aPointerOrientation) {
  ScreenIntPoint pointerScreenPoint = ViewAs<ScreenPixel>(
      aPoint, PixelCastJustification::LayoutDeviceIsScreenForBounds);

  // We can't dispatch *aState directly because (a) dispatching
  // it might inadvertently modify it and (b) in the case of touchend or
  // touchcancel events aState will hold the touches that are
  // still down whereas the input dispatched needs to hold the removed
  // touch(es). We use |inputToDispatch| for this purpose.
  MultiTouchInput inputToDispatch;
  inputToDispatch.mInputType = MULTITOUCH_INPUT;
  inputToDispatch.mTimeStamp = aTimeStamp;

  int32_t index = aState->IndexOfTouch((int32_t)aPointerId);
  if (aPointerState == TOUCH_CONTACT) {
    if (index >= 0) {
      // found an existing touch point, update it
      SingleTouchData& point = aState->mTouches[index];
      point.mScreenPoint = pointerScreenPoint;
      point.mRotationAngle = (float)aPointerOrientation;
      point.mForce = (float)aPointerPressure;
      inputToDispatch.mType = MultiTouchInput::MULTITOUCH_MOVE;
    } else {
      // new touch point, add it
      aState->mTouches.AppendElement(SingleTouchData(
          (int32_t)aPointerId, pointerScreenPoint, ScreenSize(0, 0),
          (float)aPointerOrientation, (float)aPointerPressure));
      inputToDispatch.mType = MultiTouchInput::MULTITOUCH_START;
    }
    inputToDispatch.mTouches = aState->mTouches;
  } else {
    MOZ_ASSERT(aPointerState == TOUCH_REMOVE || aPointerState == TOUCH_CANCEL);
    // a touch point is being lifted, so remove it from the stored list
    if (index >= 0) {
      aState->mTouches.RemoveElementAt(index);
    }
    inputToDispatch.mType =
        (aPointerState == TOUCH_REMOVE ? MultiTouchInput::MULTITOUCH_END
                                       : MultiTouchInput::MULTITOUCH_CANCEL);
    inputToDispatch.mTouches.AppendElement(SingleTouchData(
        (int32_t)aPointerId, pointerScreenPoint, ScreenSize(0, 0),
        (float)aPointerOrientation, (float)aPointerPressure));
  }

  return inputToDispatch;
}

void nsBaseWidget::NotifyLiveResizeStarted() {
  // If we have mLiveResizeListeners already non-empty, we should notify those
  // listeners that the resize stopped before starting anew. In theory this
  // should never happen because we shouldn't get nested live resize actions.
  NotifyLiveResizeStopped();
  MOZ_ASSERT(mLiveResizeListeners.IsEmpty());

  // If we can get the active remote tab for the current widget, suppress
  // the displayport on it during the live resize.
  if (!mWidgetListener) {
    return;
  }
  nsCOMPtr<nsIAppWindow> appWindow = mWidgetListener->GetAppWindow();
  if (!appWindow) {
    return;
  }
  mLiveResizeListeners = appWindow->GetLiveResizeListeners();
  for (uint32_t i = 0; i < mLiveResizeListeners.Length(); i++) {
    mLiveResizeListeners[i]->LiveResizeStarted();
  }
}

void nsBaseWidget::NotifyLiveResizeStopped() {
  if (!mLiveResizeListeners.IsEmpty()) {
    for (uint32_t i = 0; i < mLiveResizeListeners.Length(); i++) {
      mLiveResizeListeners[i]->LiveResizeStopped();
    }
    mLiveResizeListeners.Clear();
  }
}

nsresult nsBaseWidget::AsyncEnableDragDrop(bool aEnable) {
  RefPtr<nsBaseWidget> kungFuDeathGrip = this;
  return NS_DispatchToCurrentThreadQueue(
      NS_NewRunnableFunction(
          "AsyncEnableDragDropFn",
          [this, aEnable, kungFuDeathGrip]() { EnableDragDrop(aEnable); }),
      kAsyncDragDropTimeout, EventQueuePriority::Idle);
}

void nsBaseWidget::SwipeFinished() {
  if (mSwipeTracker) {
    mSwipeTracker->Destroy();
    mSwipeTracker = nullptr;
  }
}

void nsBaseWidget::ReportSwipeStarted(uint64_t aInputBlockId,
                                      bool aStartSwipe) {
  if (mSwipeEventQueue && mSwipeEventQueue->inputBlockId == aInputBlockId) {
    if (aStartSwipe) {
      PanGestureInput& startEvent = mSwipeEventQueue->queuedEvents[0];
      TrackScrollEventAsSwipe(startEvent, mSwipeEventQueue->allowedDirections,
                              aInputBlockId);
      for (size_t i = 1; i < mSwipeEventQueue->queuedEvents.Length(); i++) {
        mSwipeTracker->ProcessEvent(mSwipeEventQueue->queuedEvents[i]);
      }
    } else if (mAPZC) {
      // If the event wasn't start swipe, we need to notify it to APZ.
      mAPZC->SetBrowserGestureResponse(aInputBlockId,
                                       BrowserGestureResponse::NotConsumed);
    }
    mSwipeEventQueue = nullptr;
  }
}

void nsBaseWidget::TrackScrollEventAsSwipe(
    const mozilla::PanGestureInput& aSwipeStartEvent,
    uint32_t aAllowedDirections, uint64_t aInputBlockId) {
  // If a swipe is currently being tracked kill it -- it's been interrupted
  // by another gesture event.
  if (mSwipeTracker) {
    mSwipeTracker->CancelSwipe(aSwipeStartEvent.mTimeStamp);
    mSwipeTracker->Destroy();
    mSwipeTracker = nullptr;
  }

  uint32_t direction =
      (aSwipeStartEvent.mPanDisplacement.x > 0.0)
          ? (uint32_t)dom::SimpleGestureEvent_Binding::DIRECTION_RIGHT
          : (uint32_t)dom::SimpleGestureEvent_Binding::DIRECTION_LEFT;

  mSwipeTracker =
      new SwipeTracker(*this, aSwipeStartEvent, aAllowedDirections, direction);

  if (!mAPZC) {
    mCurrentPanGestureBelongsToSwipe = true;
  } else {
    // Now SwipeTracker has started consuming pan events, notify it to APZ so
    // that APZ can discard queued events.
    mAPZC->SetBrowserGestureResponse(aInputBlockId,
                                     BrowserGestureResponse::Consumed);
  }
}

nsBaseWidget::SwipeInfo nsBaseWidget::SendMayStartSwipe(
    const mozilla::PanGestureInput& aSwipeStartEvent) {
  nsCOMPtr<nsIWidget> kungFuDeathGrip(this);

  uint32_t direction =
      (aSwipeStartEvent.mPanDisplacement.x > 0.0)
          ? (uint32_t)dom::SimpleGestureEvent_Binding::DIRECTION_RIGHT
          : (uint32_t)dom::SimpleGestureEvent_Binding::DIRECTION_LEFT;

  // We're ready to start the animation. Tell Gecko about it, and at the same
  // time ask it if it really wants to start an animation for this event.
  // This event also reports back the directions that we can swipe in.
  LayoutDeviceIntPoint position = RoundedToInt(aSwipeStartEvent.mPanStartPoint *
                                               ScreenToLayoutDeviceScale(1));
  WidgetSimpleGestureEvent geckoEvent = SwipeTracker::CreateSwipeGestureEvent(
      eSwipeGestureMayStart, this, position, aSwipeStartEvent.mTimeStamp);
  geckoEvent.mDirection = direction;
  geckoEvent.mDelta = 0.0;
  geckoEvent.mAllowedDirections = 0;
  bool shouldStartSwipe =
      DispatchWindowEvent(geckoEvent);  // event cancelled == swipe should start

  SwipeInfo result = {shouldStartSwipe, geckoEvent.mAllowedDirections};
  return result;
}

WidgetWheelEvent nsBaseWidget::MayStartSwipeForAPZ(
    const PanGestureInput& aPanInput, const APZEventResult& aApzResult) {
  WidgetWheelEvent event = aPanInput.ToWidgetEvent(this);

  // Ignore swipe-to-navigation in PiP window.
  if (mIsPIPWindow) {
    return event;
  }

  if (aPanInput.AllowsSwipe()) {
    SwipeInfo swipeInfo = SendMayStartSwipe(aPanInput);
    event.mCanTriggerSwipe = swipeInfo.wantsSwipe;
    if (swipeInfo.wantsSwipe) {
      if (aApzResult.GetStatus() == nsEventStatus_eIgnore) {
        // APZ has determined and that scrolling horizontally in the
        // requested direction is impossible, so it didn't do any
        // scrolling for the event.
        // We know now that MayStartSwipe wants a swipe, so we can start
        // the swipe now.
        TrackScrollEventAsSwipe(aPanInput, swipeInfo.allowedDirections,
                                aApzResult.mInputBlockId);
      } else if (!aApzResult.GetHandledResult() ||
                 !aApzResult.GetHandledResult()->IsHandledByRoot()) {
        // We don't know whether this event can start a swipe, so we need
        // to queue up events and wait for a call to ReportSwipeStarted.
        // APZ might already have started scrolling in response to the
        // event if it knew that it's the right thing to do. In that case
        // we'll still get a call to ReportSwipeStarted, and we will
        // discard the queued events at that point.
        mSwipeEventQueue = MakeUnique<SwipeEventQueue>(
            swipeInfo.allowedDirections, aApzResult.mInputBlockId);
      }
    } else {
      // Inform that the browser gesture didn't use the pan event (pan-start
      // precisely), so that APZ can now start using the event for
      // scrolling/overscrolling.
      mAPZC->SetBrowserGestureResponse(aApzResult.mInputBlockId,
                                       BrowserGestureResponse::NotConsumed);
    }
  }

  if (mSwipeEventQueue &&
      mSwipeEventQueue->inputBlockId == aApzResult.mInputBlockId) {
    mSwipeEventQueue->queuedEvents.AppendElement(aPanInput);
  }

  return event;
}

bool nsBaseWidget::MayStartSwipeForNonAPZ(const PanGestureInput& aPanInput) {
  // Ignore swipe-to-navigation in PiP window.
  if (mIsPIPWindow) {
    return false;
  }

  if (aPanInput.mType == PanGestureInput::PANGESTURE_MAYSTART ||
      aPanInput.mType == PanGestureInput::PANGESTURE_START) {
    mCurrentPanGestureBelongsToSwipe = false;
  }
  if (mCurrentPanGestureBelongsToSwipe) {
    // Ignore this event. It's a momentum event from a scroll gesture
    // that was processed as a swipe, and the swipe animation has
    // already finished (so mSwipeTracker is already null).
    MOZ_ASSERT(aPanInput.IsMomentum(),
               "If the fingers are still on the touchpad, we should still have "
               "a SwipeTracker, "
               "and it should have consumed this event.");
    return true;
  }

  if (!aPanInput.MayTriggerSwipe()) {
    return false;
  }

  SwipeInfo swipeInfo = SendMayStartSwipe(aPanInput);

  // We're in the non-APZ case here, but we still want to know whether
  // the event was routed to a child process, so we use InputAPZContext
  // to get that piece of information.
  ScrollableLayerGuid guid;
  uint64_t blockId = 0;
  InputAPZContext context(guid, blockId, nsEventStatus_eIgnore);

  WidgetWheelEvent event = aPanInput.ToWidgetEvent(this);
  event.mCanTriggerSwipe = swipeInfo.wantsSwipe;
  nsEventStatus status;
  DispatchEvent(&event, status);
  if (swipeInfo.wantsSwipe) {
    if (context.WasRoutedToChildProcess()) {
      // We don't know whether this event can start a swipe, so we need
      // to queue up events and wait for a call to ReportSwipeStarted.
      mSwipeEventQueue =
          MakeUnique<SwipeEventQueue>(swipeInfo.allowedDirections, blockId);
    } else if (event.TriggersSwipe()) {
      TrackScrollEventAsSwipe(aPanInput, swipeInfo.allowedDirections, blockId);
    }
  }

  if (mSwipeEventQueue && mSwipeEventQueue->inputBlockId == 0) {
    mSwipeEventQueue->queuedEvents.AppendElement(aPanInput);
  }

  return true;
}

const IMENotificationRequests& nsIWidget::IMENotificationRequestsRef() {
  TextEventDispatcher* dispatcher = GetTextEventDispatcher();
  return dispatcher->IMENotificationRequestsRef();
}

void nsIWidget::PostHandleKeyEvent(mozilla::WidgetKeyboardEvent* aEvent) {}

bool nsIWidget::GetEditCommands(NativeKeyBindingsType aType,
                                const WidgetKeyboardEvent& aEvent,
                                nsTArray<CommandInt>& aCommands) {
  MOZ_ASSERT(aEvent.IsTrusted());
  MOZ_ASSERT(aCommands.IsEmpty());
  return true;
}

already_AddRefed<nsIBidiKeyboard> nsIWidget::CreateBidiKeyboard() {
  if (XRE_IsContentProcess()) {
    return CreateBidiKeyboardContentProcess();
  }
  return CreateBidiKeyboardInner();
}

#ifdef ANDROID
already_AddRefed<nsIBidiKeyboard> nsIWidget::CreateBidiKeyboardInner() {
  // no bidi keyboard implementation
  return nullptr;
}
#endif

namespace mozilla::widget {

const char* ToChar(InputContext::Origin aOrigin) {
  switch (aOrigin) {
    case InputContext::ORIGIN_MAIN:
      return "ORIGIN_MAIN";
    case InputContext::ORIGIN_CONTENT:
      return "ORIGIN_CONTENT";
    default:
      return "Unexpected value";
  }
}

const char* ToChar(IMEMessage aIMEMessage) {
  switch (aIMEMessage) {
    case NOTIFY_IME_OF_NOTHING:
      return "NOTIFY_IME_OF_NOTHING";
    case NOTIFY_IME_OF_FOCUS:
      return "NOTIFY_IME_OF_FOCUS";
    case NOTIFY_IME_OF_BLUR:
      return "NOTIFY_IME_OF_BLUR";
    case NOTIFY_IME_OF_SELECTION_CHANGE:
      return "NOTIFY_IME_OF_SELECTION_CHANGE";
    case NOTIFY_IME_OF_TEXT_CHANGE:
      return "NOTIFY_IME_OF_TEXT_CHANGE";
    case NOTIFY_IME_OF_COMPOSITION_EVENT_HANDLED:
      return "NOTIFY_IME_OF_COMPOSITION_EVENT_HANDLED";
    case NOTIFY_IME_OF_POSITION_CHANGE:
      return "NOTIFY_IME_OF_POSITION_CHANGE";
    case NOTIFY_IME_OF_MOUSE_BUTTON_EVENT:
      return "NOTIFY_IME_OF_MOUSE_BUTTON_EVENT";
    case REQUEST_TO_COMMIT_COMPOSITION:
      return "REQUEST_TO_COMMIT_COMPOSITION";
    case REQUEST_TO_CANCEL_COMPOSITION:
      return "REQUEST_TO_CANCEL_COMPOSITION";
    default:
      return "Unexpected value";
  }
}

void NativeIMEContext::Init(nsIWidget* aWidget) {
  if (!aWidget) {
    mRawNativeIMEContext = reinterpret_cast<uintptr_t>(nullptr);
    mOriginProcessID = static_cast<uint64_t>(-1);
    return;
  }
  if (!XRE_IsContentProcess()) {
    mRawNativeIMEContext = reinterpret_cast<uintptr_t>(
        aWidget->GetNativeData(NS_RAW_NATIVE_IME_CONTEXT));
    mOriginProcessID = 0;
    return;
  }
  // If this is created in a child process, aWidget is an instance of
  // PuppetWidget which doesn't support NS_RAW_NATIVE_IME_CONTEXT.
  // Instead of that PuppetWidget::GetNativeIMEContext() returns cached
  // native IME context of the parent process.
  *this = aWidget->GetNativeIMEContext();
}

void NativeIMEContext::InitWithRawNativeIMEContext(void* aRawNativeIMEContext) {
  if (NS_WARN_IF(!aRawNativeIMEContext)) {
    mRawNativeIMEContext = reinterpret_cast<uintptr_t>(nullptr);
    mOriginProcessID = static_cast<uint64_t>(-1);
    return;
  }
  mRawNativeIMEContext = reinterpret_cast<uintptr_t>(aRawNativeIMEContext);
  mOriginProcessID =
      XRE_IsContentProcess() ? ContentChild::GetSingleton()->GetID() : 0;
}

void IMENotification::TextChangeDataBase::MergeWith(
    const IMENotification::TextChangeDataBase& aOther) {
  MOZ_ASSERT(aOther.IsValid(), "Merging data must store valid data");
  MOZ_ASSERT(aOther.mStartOffset <= aOther.mRemovedEndOffset,
             "end of removed text must be same or larger than start");
  MOZ_ASSERT(aOther.mStartOffset <= aOther.mAddedEndOffset,
             "end of added text must be same or larger than start");

  if (!IsValid()) {
    *this = aOther;
    return;
  }

  // |mStartOffset| and |mRemovedEndOffset| represent all replaced or removed
  // text ranges.  I.e., mStartOffset should be the smallest offset of all
  // modified text ranges in old text.  |mRemovedEndOffset| should be the
  // largest end offset in old text of all modified text ranges.
  // |mAddedEndOffset| represents the end offset of all inserted text ranges.
  // I.e., only this is an offset in new text.
  // In other words, between mStartOffset and |mRemovedEndOffset| of the
  // premodified text was already removed.  And some text whose length is
  // |mAddedEndOffset - mStartOffset| is inserted to |mStartOffset|.  I.e.,
  // this allows IME to mark dirty the modified text range with |mStartOffset|
  // and |mRemovedEndOffset| if IME stores all text of the focused editor and
  // to compute new text length with |mAddedEndOffset| and |mRemovedEndOffset|.
  // Additionally, IME can retrieve only the text between |mStartOffset| and
  // |mAddedEndOffset| for updating stored text.

  // For comparing new and old |mStartOffset|/|mRemovedEndOffset| values, they
  // should be adjusted to be in same text. The |newData.mStartOffset| and
  // |newData.mRemovedEndOffset| should be computed as in old text because
  // |mStartOffset| and |mRemovedEndOffset| represent the modified text range
  // in the old text but even if some text before the values of the newData
  // has already been modified, the values don't include the changes.

  // For comparing new and old |mAddedEndOffset| values, they should be
  // adjusted to be in same text.  The |oldData.mAddedEndOffset| should be
  // computed as in the new text because |mAddedEndOffset| indicates the end
  // offset of inserted text in the new text but |oldData.mAddedEndOffset|
  // doesn't include any changes of the text before |newData.mAddedEndOffset|.

  const TextChangeDataBase& newData = aOther;
  const TextChangeDataBase oldData = *this;

  // mCausedOnlyByComposition should be true only when all changes are caused
  // by composition.
  mCausedOnlyByComposition =
      newData.mCausedOnlyByComposition && oldData.mCausedOnlyByComposition;

  // mIncludingChangesWithoutComposition should be true if at least one of
  // merged changes occurred without composition.
  mIncludingChangesWithoutComposition =
      newData.mIncludingChangesWithoutComposition ||
      oldData.mIncludingChangesWithoutComposition;

  // mIncludingChangesDuringComposition should be true when at least one of
  // the merged non-composition changes occurred during the latest composition.
  if (!newData.mCausedOnlyByComposition &&
      !newData.mIncludingChangesDuringComposition) {
    MOZ_ASSERT(newData.mIncludingChangesWithoutComposition);
    MOZ_ASSERT(mIncludingChangesWithoutComposition);
    // If new change is neither caused by composition nor occurred during
    // composition, set mIncludingChangesDuringComposition to false because
    // IME doesn't want outdated text changes as text change during current
    // composition.
    mIncludingChangesDuringComposition = false;
  } else {
    // Otherwise, set mIncludingChangesDuringComposition to true if either
    // oldData or newData includes changes during composition.
    mIncludingChangesDuringComposition =
        newData.mIncludingChangesDuringComposition ||
        oldData.mIncludingChangesDuringComposition;
  }

  if (newData.mStartOffset >= oldData.mAddedEndOffset) {
    // Case 1:
    // If new start is after old end offset of added text, it means that text
    // after the modified range is modified.  Like:
    // added range of old change:             +----------+
    // removed range of new change:                           +----------+
    // So, the old start offset is always the smaller offset.
    mStartOffset = oldData.mStartOffset;
    // The new end offset of removed text is moved by the old change and we
    // need to cancel the move of the old change for comparing the offsets in
    // same text because it doesn't make sensce to compare offsets in different
    // text.
    uint32_t newRemovedEndOffsetInOldText =
        newData.mRemovedEndOffset - oldData.Difference();
    mRemovedEndOffset =
        std::max(newRemovedEndOffsetInOldText, oldData.mRemovedEndOffset);
    // The new end offset of added text is always the larger offset.
    mAddedEndOffset = newData.mAddedEndOffset;
    return;
  }

  if (newData.mStartOffset >= oldData.mStartOffset) {
    // If new start is in the modified range, it means that new data changes
    // a part or all of the range.
    mStartOffset = oldData.mStartOffset;
    if (newData.mRemovedEndOffset >= oldData.mAddedEndOffset) {
      // Case 2:
      // If new end of removed text is greater than old end of added text, it
      // means that all or a part of modified range modified again and text
      // after the modified range is also modified.  Like:
      // added range of old change:             +----------+
      // removed range of new change:                   +----------+
      // So, the new removed end offset is moved by the old change and we need
      // to cancel the move of the old change for comparing the offsets in the
      // same text because it doesn't make sense to compare the offsets in
      // different text.
      uint32_t newRemovedEndOffsetInOldText =
          newData.mRemovedEndOffset - oldData.Difference();
      mRemovedEndOffset =
          std::max(newRemovedEndOffsetInOldText, oldData.mRemovedEndOffset);
      // The old end of added text is replaced by new change. So, it should be
      // same as the new start.  On the other hand, the new added end offset is
      // always same or larger.  Therefore, the merged end offset of added
      // text should be the new end offset of added text.
      mAddedEndOffset = newData.mAddedEndOffset;
      return;
    }

    // Case 3:
    // If new end of removed text is less than old end of added text, it means
    // that only a part of the modified range is modified again.  Like:
    // added range of old change:             +------------+
    // removed range of new change:               +-----+
    // So, the new end offset of removed text should be same as the old end
    // offset of removed text.  Therefore, the merged end offset of removed
    // text should be the old text change's |mRemovedEndOffset|.
    mRemovedEndOffset = oldData.mRemovedEndOffset;
    // The old end of added text is moved by new change.  So, we need to cancel
    // the move of the new change for comparing the offsets in same text.
    uint32_t oldAddedEndOffsetInNewText =
        oldData.mAddedEndOffset + newData.Difference();
    mAddedEndOffset =
        std::max(newData.mAddedEndOffset, oldAddedEndOffsetInNewText);
    return;
  }

  if (newData.mRemovedEndOffset >= oldData.mStartOffset) {
    // If new end of removed text is greater than old start (and new start is
    // less than old start), it means that a part of modified range is modified
    // again and some new text before the modified range is also modified.
    MOZ_ASSERT(newData.mStartOffset < oldData.mStartOffset,
               "new start offset should be less than old one here");
    mStartOffset = newData.mStartOffset;
    if (newData.mRemovedEndOffset >= oldData.mAddedEndOffset) {
      // Case 4:
      // If new end of removed text is greater than old end of added text, it
      // means that all modified text and text after the modified range is
      // modified.  Like:
      // added range of old change:             +----------+
      // removed range of new change:        +------------------+
      // So, the new end of removed text is moved by the old change.  Therefore,
      // we need to cancel the move of the old change for comparing the offsets
      // in same text because it doesn't make sense to compare the offsets in
      // different text.
      uint32_t newRemovedEndOffsetInOldText =
          newData.mRemovedEndOffset - oldData.Difference();
      mRemovedEndOffset =
          std::max(newRemovedEndOffsetInOldText, oldData.mRemovedEndOffset);
      // The old end of added text is replaced by new change.  So, the old end
      // offset of added text is same as new text change's start offset.  Then,
      // new change's end offset of added text is always same or larger than
      // it.  Therefore, merged end offset of added text is always the new end
      // offset of added text.
      mAddedEndOffset = newData.mAddedEndOffset;
      return;
    }

    // Case 5:
    // If new end of removed text is less than old end of added text, it
    // means that only a part of the modified range is modified again.  Like:
    // added range of old change:             +----------+
    // removed range of new change:      +----------+
    // So, the new end of removed text should be same as old end of removed
    // text for preventing end of removed text to be modified.  Therefore,
    // merged end offset of removed text is always the old end offset of removed
    // text.
    mRemovedEndOffset = oldData.mRemovedEndOffset;
    // The old end of added text is moved by this change.  So, we need to
    // cancel the move of the new change for comparing the offsets in same text
    // because it doesn't make sense to compare the offsets in different text.
    uint32_t oldAddedEndOffsetInNewText =
        oldData.mAddedEndOffset + newData.Difference();
    mAddedEndOffset =
        std::max(newData.mAddedEndOffset, oldAddedEndOffsetInNewText);
    return;
  }

  // Case 6:
  // Otherwise, i.e., both new end of added text and new start are less than
  // old start, text before the modified range is modified.  Like:
  // added range of old change:                  +----------+
  // removed range of new change: +----------+
  MOZ_ASSERT(newData.mStartOffset < oldData.mStartOffset,
             "new start offset should be less than old one here");
  mStartOffset = newData.mStartOffset;
  MOZ_ASSERT(newData.mRemovedEndOffset < oldData.mRemovedEndOffset,
             "new removed end offset should be less than old one here");
  mRemovedEndOffset = oldData.mRemovedEndOffset;
  // The end of added text should be adjusted with the new difference.
  uint32_t oldAddedEndOffsetInNewText =
      oldData.mAddedEndOffset + newData.Difference();
  mAddedEndOffset =
      std::max(newData.mAddedEndOffset, oldAddedEndOffsetInNewText);
}

#ifdef DEBUG

// Let's test the code of merging multiple text change data in debug build
// and crash if one of them fails because this feature is very complex but
// cannot be tested with mochitest.
void IMENotification::TextChangeDataBase::Test() {
  static bool gTestTextChangeEvent = true;
  if (!gTestTextChangeEvent) {
    return;
  }
  gTestTextChangeEvent = false;

  /****************************************************************************
   * Case 1
   ****************************************************************************/

  // Appending text
  MergeWith(TextChangeData(10, 10, 20, false, false));
  MergeWith(TextChangeData(20, 20, 35, false, false));
  MOZ_ASSERT(mStartOffset == 10,
             "Test 1-1-1: mStartOffset should be the first offset");
  MOZ_ASSERT(
      mRemovedEndOffset == 10,  // 20 - (20 - 10)
      "Test 1-1-2: mRemovedEndOffset should be the first end of removed text");
  MOZ_ASSERT(
      mAddedEndOffset == 35,
      "Test 1-1-3: mAddedEndOffset should be the last end of added text");
  Clear();

  // Removing text (longer line -> shorter line)
  MergeWith(TextChangeData(10, 20, 10, false, false));
  MergeWith(TextChangeData(10, 30, 10, false, false));
  MOZ_ASSERT(mStartOffset == 10,
             "Test 1-2-1: mStartOffset should be the first offset");
  MOZ_ASSERT(mRemovedEndOffset == 40,  // 30 + (10 - 20)
             "Test 1-2-2: mRemovedEndOffset should be the the last end of "
             "removed text "
             "with already removed length");
  MOZ_ASSERT(
      mAddedEndOffset == 10,
      "Test 1-2-3: mAddedEndOffset should be the last end of added text");
  Clear();

  // Removing text (shorter line -> longer line)
  MergeWith(TextChangeData(10, 20, 10, false, false));
  MergeWith(TextChangeData(10, 15, 10, false, false));
  MOZ_ASSERT(mStartOffset == 10,
             "Test 1-3-1: mStartOffset should be the first offset");
  MOZ_ASSERT(mRemovedEndOffset == 25,  // 15 + (10 - 20)
             "Test 1-3-2: mRemovedEndOffset should be the the last end of "
             "removed text "
             "with already removed length");
  MOZ_ASSERT(
      mAddedEndOffset == 10,
      "Test 1-3-3: mAddedEndOffset should be the last end of added text");
  Clear();

  // Appending text at different point (not sure if actually occurs)
  MergeWith(TextChangeData(10, 10, 20, false, false));
  MergeWith(TextChangeData(55, 55, 60, false, false));
  MOZ_ASSERT(mStartOffset == 10,
             "Test 1-4-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(
      mRemovedEndOffset == 45,  // 55 - (10 - 20)
      "Test 1-4-2: mRemovedEndOffset should be the the largest end of removed "
      "text without already added length");
  MOZ_ASSERT(
      mAddedEndOffset == 60,
      "Test 1-4-3: mAddedEndOffset should be the last end of added text");
  Clear();

  // Removing text at different point (not sure if actually occurs)
  MergeWith(TextChangeData(10, 20, 10, false, false));
  MergeWith(TextChangeData(55, 68, 55, false, false));
  MOZ_ASSERT(mStartOffset == 10,
             "Test 1-5-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(
      mRemovedEndOffset == 78,  // 68 - (10 - 20)
      "Test 1-5-2: mRemovedEndOffset should be the the largest end of removed "
      "text with already removed length");
  MOZ_ASSERT(
      mAddedEndOffset == 55,
      "Test 1-5-3: mAddedEndOffset should be the largest end of added text");
  Clear();

  // Replacing text and append text (becomes longer)
  MergeWith(TextChangeData(30, 35, 32, false, false));
  MergeWith(TextChangeData(32, 32, 40, false, false));
  MOZ_ASSERT(mStartOffset == 30,
             "Test 1-6-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(
      mRemovedEndOffset == 35,  // 32 - (32 - 35)
      "Test 1-6-2: mRemovedEndOffset should be the the first end of removed "
      "text");
  MOZ_ASSERT(
      mAddedEndOffset == 40,
      "Test 1-6-3: mAddedEndOffset should be the last end of added text");
  Clear();

  // Replacing text and append text (becomes shorter)
  MergeWith(TextChangeData(30, 35, 32, false, false));
  MergeWith(TextChangeData(32, 32, 33, false, false));
  MOZ_ASSERT(mStartOffset == 30,
             "Test 1-7-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(
      mRemovedEndOffset == 35,  // 32 - (32 - 35)
      "Test 1-7-2: mRemovedEndOffset should be the the first end of removed "
      "text");
  MOZ_ASSERT(
      mAddedEndOffset == 33,
      "Test 1-7-3: mAddedEndOffset should be the last end of added text");
  Clear();

  // Removing text and replacing text after first range (not sure if actually
  // occurs)
  MergeWith(TextChangeData(30, 35, 30, false, false));
  MergeWith(TextChangeData(32, 34, 48, false, false));
  MOZ_ASSERT(mStartOffset == 30,
             "Test 1-8-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(mRemovedEndOffset == 39,  // 34 - (30 - 35)
             "Test 1-8-2: mRemovedEndOffset should be the the first end of "
             "removed text "
             "without already removed text");
  MOZ_ASSERT(
      mAddedEndOffset == 48,
      "Test 1-8-3: mAddedEndOffset should be the last end of added text");
  Clear();

  // Removing text and replacing text after first range (not sure if actually
  // occurs)
  MergeWith(TextChangeData(30, 35, 30, false, false));
  MergeWith(TextChangeData(32, 38, 36, false, false));
  MOZ_ASSERT(mStartOffset == 30,
             "Test 1-9-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(mRemovedEndOffset == 43,  // 38 - (30 - 35)
             "Test 1-9-2: mRemovedEndOffset should be the the first end of "
             "removed text "
             "without already removed text");
  MOZ_ASSERT(
      mAddedEndOffset == 36,
      "Test 1-9-3: mAddedEndOffset should be the last end of added text");
  Clear();

  /****************************************************************************
   * Case 2
   ****************************************************************************/

  // Replacing text in around end of added text (becomes shorter) (not sure
  // if actually occurs)
  MergeWith(TextChangeData(50, 50, 55, false, false));
  MergeWith(TextChangeData(53, 60, 54, false, false));
  MOZ_ASSERT(mStartOffset == 50,
             "Test 2-1-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(mRemovedEndOffset == 55,  // 60 - (55 - 50)
             "Test 2-1-2: mRemovedEndOffset should be the the last end of "
             "removed text "
             "without already added text length");
  MOZ_ASSERT(
      mAddedEndOffset == 54,
      "Test 2-1-3: mAddedEndOffset should be the last end of added text");
  Clear();

  // Replacing text around end of added text (becomes longer) (not sure
  // if actually occurs)
  MergeWith(TextChangeData(50, 50, 55, false, false));
  MergeWith(TextChangeData(54, 62, 68, false, false));
  MOZ_ASSERT(mStartOffset == 50,
             "Test 2-2-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(mRemovedEndOffset == 57,  // 62 - (55 - 50)
             "Test 2-2-2: mRemovedEndOffset should be the the last end of "
             "removed text "
             "without already added text length");
  MOZ_ASSERT(
      mAddedEndOffset == 68,
      "Test 2-2-3: mAddedEndOffset should be the last end of added text");
  Clear();

  // Replacing text around end of replaced text (became shorter) (not sure if
  // actually occurs)
  MergeWith(TextChangeData(36, 48, 45, false, false));
  MergeWith(TextChangeData(43, 50, 49, false, false));
  MOZ_ASSERT(mStartOffset == 36,
             "Test 2-3-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(mRemovedEndOffset == 53,  // 50 - (45 - 48)
             "Test 2-3-2: mRemovedEndOffset should be the the last end of "
             "removed text "
             "without already removed text length");
  MOZ_ASSERT(
      mAddedEndOffset == 49,
      "Test 2-3-3: mAddedEndOffset should be the last end of added text");
  Clear();

  // Replacing text around end of replaced text (became longer) (not sure if
  // actually occurs)
  MergeWith(TextChangeData(36, 52, 53, false, false));
  MergeWith(TextChangeData(43, 68, 61, false, false));
  MOZ_ASSERT(mStartOffset == 36,
             "Test 2-4-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(mRemovedEndOffset == 67,  // 68 - (53 - 52)
             "Test 2-4-2: mRemovedEndOffset should be the the last end of "
             "removed text "
             "without already added text length");
  MOZ_ASSERT(
      mAddedEndOffset == 61,
      "Test 2-4-3: mAddedEndOffset should be the last end of added text");
  Clear();

  /****************************************************************************
   * Case 3
   ****************************************************************************/

  // Appending text in already added text (not sure if actually occurs)
  MergeWith(TextChangeData(10, 10, 20, false, false));
  MergeWith(TextChangeData(15, 15, 30, false, false));
  MOZ_ASSERT(mStartOffset == 10,
             "Test 3-1-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(mRemovedEndOffset == 10,
             "Test 3-1-2: mRemovedEndOffset should be the the first end of "
             "removed text");
  MOZ_ASSERT(
      mAddedEndOffset == 35,  // 20 + (30 - 15)
      "Test 3-1-3: mAddedEndOffset should be the first end of added text with "
      "added text length by the new change");
  Clear();

  // Replacing text in added text (not sure if actually occurs)
  MergeWith(TextChangeData(50, 50, 55, false, false));
  MergeWith(TextChangeData(52, 53, 56, false, false));
  MOZ_ASSERT(mStartOffset == 50,
             "Test 3-2-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(mRemovedEndOffset == 50,
             "Test 3-2-2: mRemovedEndOffset should be the the first end of "
             "removed text");
  MOZ_ASSERT(
      mAddedEndOffset == 58,  // 55 + (56 - 53)
      "Test 3-2-3: mAddedEndOffset should be the first end of added text with "
      "added text length by the new change");
  Clear();

  // Replacing text in replaced text (became shorter) (not sure if actually
  // occurs)
  MergeWith(TextChangeData(36, 48, 45, false, false));
  MergeWith(TextChangeData(37, 38, 50, false, false));
  MOZ_ASSERT(mStartOffset == 36,
             "Test 3-3-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(mRemovedEndOffset == 48,
             "Test 3-3-2: mRemovedEndOffset should be the the first end of "
             "removed text");
  MOZ_ASSERT(
      mAddedEndOffset == 57,  // 45 + (50 - 38)
      "Test 3-3-3: mAddedEndOffset should be the first end of added text with "
      "added text length by the new change");
  Clear();

  // Replacing text in replaced text (became longer) (not sure if actually
  // occurs)
  MergeWith(TextChangeData(32, 48, 53, false, false));
  MergeWith(TextChangeData(43, 50, 52, false, false));
  MOZ_ASSERT(mStartOffset == 32,
             "Test 3-4-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(mRemovedEndOffset == 48,
             "Test 3-4-2: mRemovedEndOffset should be the the last end of "
             "removed text "
             "without already added text length");
  MOZ_ASSERT(
      mAddedEndOffset == 55,  // 53 + (52 - 50)
      "Test 3-4-3: mAddedEndOffset should be the first end of added text with "
      "added text length by the new change");
  Clear();

  // Replacing text in replaced text (became shorter) (not sure if actually
  // occurs)
  MergeWith(TextChangeData(36, 48, 50, false, false));
  MergeWith(TextChangeData(37, 49, 47, false, false));
  MOZ_ASSERT(mStartOffset == 36,
             "Test 3-5-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(
      mRemovedEndOffset == 48,
      "Test 3-5-2: mRemovedEndOffset should be the the first end of removed "
      "text");
  MOZ_ASSERT(mAddedEndOffset == 48,  // 50 + (47 - 49)
             "Test 3-5-3: mAddedEndOffset should be the first end of added "
             "text without "
             "removed text length by the new change");
  Clear();

  // Replacing text in replaced text (became longer) (not sure if actually
  // occurs)
  MergeWith(TextChangeData(32, 48, 53, false, false));
  MergeWith(TextChangeData(43, 50, 47, false, false));
  MOZ_ASSERT(mStartOffset == 32,
             "Test 3-6-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(mRemovedEndOffset == 48,
             "Test 3-6-2: mRemovedEndOffset should be the the last end of "
             "removed text "
             "without already added text length");
  MOZ_ASSERT(mAddedEndOffset == 50,  // 53 + (47 - 50)
             "Test 3-6-3: mAddedEndOffset should be the first end of added "
             "text without "
             "removed text length by the new change");
  Clear();

  /****************************************************************************
   * Case 4
   ****************************************************************************/

  // Replacing text all of already append text (not sure if actually occurs)
  MergeWith(TextChangeData(50, 50, 55, false, false));
  MergeWith(TextChangeData(44, 66, 68, false, false));
  MOZ_ASSERT(mStartOffset == 44,
             "Test 4-1-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(mRemovedEndOffset == 61,  // 66 - (55 - 50)
             "Test 4-1-2: mRemovedEndOffset should be the the last end of "
             "removed text "
             "without already added text length");
  MOZ_ASSERT(
      mAddedEndOffset == 68,
      "Test 4-1-3: mAddedEndOffset should be the last end of added text");
  Clear();

  // Replacing text around a point in which text was removed (not sure if
  // actually occurs)
  MergeWith(TextChangeData(50, 62, 50, false, false));
  MergeWith(TextChangeData(44, 66, 68, false, false));
  MOZ_ASSERT(mStartOffset == 44,
             "Test 4-2-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(mRemovedEndOffset == 78,  // 66 - (50 - 62)
             "Test 4-2-2: mRemovedEndOffset should be the the last end of "
             "removed text "
             "without already removed text length");
  MOZ_ASSERT(
      mAddedEndOffset == 68,
      "Test 4-2-3: mAddedEndOffset should be the last end of added text");
  Clear();

  // Replacing text all replaced text (became shorter) (not sure if actually
  // occurs)
  MergeWith(TextChangeData(50, 62, 60, false, false));
  MergeWith(TextChangeData(49, 128, 130, false, false));
  MOZ_ASSERT(mStartOffset == 49,
             "Test 4-3-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(mRemovedEndOffset == 130,  // 128 - (60 - 62)
             "Test 4-3-2: mRemovedEndOffset should be the the last end of "
             "removed text "
             "without already removed text length");
  MOZ_ASSERT(
      mAddedEndOffset == 130,
      "Test 4-3-3: mAddedEndOffset should be the last end of added text");
  Clear();

  // Replacing text all replaced text (became longer) (not sure if actually
  // occurs)
  MergeWith(TextChangeData(50, 61, 73, false, false));
  MergeWith(TextChangeData(44, 100, 50, false, false));
  MOZ_ASSERT(mStartOffset == 44,
             "Test 4-4-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(mRemovedEndOffset == 88,  // 100 - (73 - 61)
             "Test 4-4-2: mRemovedEndOffset should be the the last end of "
             "removed text "
             "with already added text length");
  MOZ_ASSERT(
      mAddedEndOffset == 50,
      "Test 4-4-3: mAddedEndOffset should be the last end of added text");
  Clear();

  /****************************************************************************
   * Case 5
   ****************************************************************************/

  // Replacing text around start of added text (not sure if actually occurs)
  MergeWith(TextChangeData(50, 50, 55, false, false));
  MergeWith(TextChangeData(48, 52, 49, false, false));
  MOZ_ASSERT(mStartOffset == 48,
             "Test 5-1-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(
      mRemovedEndOffset == 50,
      "Test 5-1-2: mRemovedEndOffset should be the the first end of removed "
      "text");
  MOZ_ASSERT(
      mAddedEndOffset == 52,  // 55 + (52 - 49)
      "Test 5-1-3: mAddedEndOffset should be the first end of added text with "
      "added text length by the new change");
  Clear();

  // Replacing text around start of replaced text (became shorter) (not sure if
  // actually occurs)
  MergeWith(TextChangeData(50, 60, 58, false, false));
  MergeWith(TextChangeData(43, 50, 48, false, false));
  MOZ_ASSERT(mStartOffset == 43,
             "Test 5-2-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(
      mRemovedEndOffset == 60,
      "Test 5-2-2: mRemovedEndOffset should be the the first end of removed "
      "text");
  MOZ_ASSERT(mAddedEndOffset == 56,  // 58 + (48 - 50)
             "Test 5-2-3: mAddedEndOffset should be the first end of added "
             "text without "
             "removed text length by the new change");
  Clear();

  // Replacing text around start of replaced text (became longer) (not sure if
  // actually occurs)
  MergeWith(TextChangeData(50, 60, 68, false, false));
  MergeWith(TextChangeData(43, 55, 53, false, false));
  MOZ_ASSERT(mStartOffset == 43,
             "Test 5-3-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(
      mRemovedEndOffset == 60,
      "Test 5-3-2: mRemovedEndOffset should be the the first end of removed "
      "text");
  MOZ_ASSERT(mAddedEndOffset == 66,  // 68 + (53 - 55)
             "Test 5-3-3: mAddedEndOffset should be the first end of added "
             "text without "
             "removed text length by the new change");
  Clear();

  // Replacing text around start of replaced text (became shorter) (not sure if
  // actually occurs)
  MergeWith(TextChangeData(50, 60, 58, false, false));
  MergeWith(TextChangeData(43, 50, 128, false, false));
  MOZ_ASSERT(mStartOffset == 43,
             "Test 5-4-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(
      mRemovedEndOffset == 60,
      "Test 5-4-2: mRemovedEndOffset should be the the first end of removed "
      "text");
  MOZ_ASSERT(
      mAddedEndOffset == 136,  // 58 + (128 - 50)
      "Test 5-4-3: mAddedEndOffset should be the first end of added text with "
      "added text length by the new change");
  Clear();

  // Replacing text around start of replaced text (became longer) (not sure if
  // actually occurs)
  MergeWith(TextChangeData(50, 60, 68, false, false));
  MergeWith(TextChangeData(43, 55, 65, false, false));
  MOZ_ASSERT(mStartOffset == 43,
             "Test 5-5-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(
      mRemovedEndOffset == 60,
      "Test 5-5-2: mRemovedEndOffset should be the the first end of removed "
      "text");
  MOZ_ASSERT(
      mAddedEndOffset == 78,  // 68 + (65 - 55)
      "Test 5-5-3: mAddedEndOffset should be the first end of added text with "
      "added text length by the new change");
  Clear();

  /****************************************************************************
   * Case 6
   ****************************************************************************/

  // Appending text before already added text (not sure if actually occurs)
  MergeWith(TextChangeData(30, 30, 45, false, false));
  MergeWith(TextChangeData(10, 10, 20, false, false));
  MOZ_ASSERT(mStartOffset == 10,
             "Test 6-1-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(
      mRemovedEndOffset == 30,
      "Test 6-1-2: mRemovedEndOffset should be the the largest end of removed "
      "text");
  MOZ_ASSERT(
      mAddedEndOffset == 55,  // 45 + (20 - 10)
      "Test 6-1-3: mAddedEndOffset should be the first end of added text with "
      "added text length by the new change");
  Clear();

  // Removing text before already removed text (not sure if actually occurs)
  MergeWith(TextChangeData(30, 35, 30, false, false));
  MergeWith(TextChangeData(10, 25, 10, false, false));
  MOZ_ASSERT(mStartOffset == 10,
             "Test 6-2-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(
      mRemovedEndOffset == 35,
      "Test 6-2-2: mRemovedEndOffset should be the the largest end of removed "
      "text");
  MOZ_ASSERT(
      mAddedEndOffset == 15,  // 30 - (25 - 10)
      "Test 6-2-3: mAddedEndOffset should be the first end of added text with "
      "removed text length by the new change");
  Clear();

  // Replacing text before already replaced text (not sure if actually occurs)
  MergeWith(TextChangeData(50, 65, 70, false, false));
  MergeWith(TextChangeData(13, 24, 15, false, false));
  MOZ_ASSERT(mStartOffset == 13,
             "Test 6-3-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(
      mRemovedEndOffset == 65,
      "Test 6-3-2: mRemovedEndOffset should be the the largest end of removed "
      "text");
  MOZ_ASSERT(mAddedEndOffset == 61,  // 70 + (15 - 24)
             "Test 6-3-3: mAddedEndOffset should be the first end of added "
             "text without "
             "removed text length by the new change");
  Clear();

  // Replacing text before already replaced text (not sure if actually occurs)
  MergeWith(TextChangeData(50, 65, 70, false, false));
  MergeWith(TextChangeData(13, 24, 36, false, false));
  MOZ_ASSERT(mStartOffset == 13,
             "Test 6-4-1: mStartOffset should be the smallest offset");
  MOZ_ASSERT(
      mRemovedEndOffset == 65,
      "Test 6-4-2: mRemovedEndOffset should be the the largest end of removed "
      "text");
  MOZ_ASSERT(mAddedEndOffset == 82,  // 70 + (36 - 24)
             "Test 6-4-3: mAddedEndOffset should be the first end of added "
             "text without "
             "removed text length by the new change");
  Clear();
}

#endif  // #ifdef DEBUG

}  // namespace mozilla::widget

#ifdef DEBUG
//////////////////////////////////////////////////////////////
//
// Convert a GUI event message code to a string.
// Makes it a lot easier to debug events.
//
// See gtk/nsWidget.cpp and windows/nsWindow.cpp
// for a DebugPrintEvent() function that uses
// this.
//
//////////////////////////////////////////////////////////////
/* static */
nsAutoString nsBaseWidget::debug_GuiEventToString(WidgetGUIEvent* aGuiEvent) {
  NS_ASSERTION(nullptr != aGuiEvent, "cmon, null gui event.");

  nsAutoString eventName(u"UNKNOWN"_ns);

#  define _ASSIGN_eventName(_value, _name) \
    case _value:                           \
      eventName.AssignLiteral(_name);      \
      break

  switch (aGuiEvent->mMessage) {
    _ASSIGN_eventName(eBlur, "eBlur");
    _ASSIGN_eventName(eDrop, "eDrop");
    _ASSIGN_eventName(eDragEnter, "eDragEnter");
    _ASSIGN_eventName(eDragExit, "eDragExit");
    _ASSIGN_eventName(eDragOver, "eDragOver");
    _ASSIGN_eventName(eEditorInput, "eEditorInput");
    _ASSIGN_eventName(eFocus, "eFocus");
    _ASSIGN_eventName(eFocusIn, "eFocusIn");
    _ASSIGN_eventName(eFocusOut, "eFocusOut");
    _ASSIGN_eventName(eFormSelect, "eFormSelect");
    _ASSIGN_eventName(eFormChange, "eFormChange");
    _ASSIGN_eventName(eFormReset, "eFormReset");
    _ASSIGN_eventName(eFormSubmit, "eFormSubmit");
    _ASSIGN_eventName(eImageAbort, "eImageAbort");
    _ASSIGN_eventName(eLoadError, "eLoadError");
    _ASSIGN_eventName(eKeyDown, "eKeyDown");
    _ASSIGN_eventName(eKeyPress, "eKeyPress");
    _ASSIGN_eventName(eKeyUp, "eKeyUp");
    _ASSIGN_eventName(eMouseEnterIntoWidget, "eMouseEnterIntoWidget");
    _ASSIGN_eventName(eMouseExitFromWidget, "eMouseExitFromWidget");
    _ASSIGN_eventName(eMouseDown, "eMouseDown");
    _ASSIGN_eventName(eMouseUp, "eMouseUp");
    _ASSIGN_eventName(eMouseClick, "eMouseClick");
    _ASSIGN_eventName(eMouseAuxClick, "eMouseAuxClick");
    _ASSIGN_eventName(eMouseDoubleClick, "eMouseDoubleClick");
    _ASSIGN_eventName(eMouseMove, "eMouseMove");
    _ASSIGN_eventName(eLoad, "eLoad");
    _ASSIGN_eventName(ePopState, "ePopState");
    _ASSIGN_eventName(eBeforeScriptExecute, "eBeforeScriptExecute");
    _ASSIGN_eventName(eAfterScriptExecute, "eAfterScriptExecute");
    _ASSIGN_eventName(eUnload, "eUnload");
    _ASSIGN_eventName(eHashChange, "eHashChange");
    _ASSIGN_eventName(eReadyStateChange, "eReadyStateChange");
    _ASSIGN_eventName(eXULBroadcast, "eXULBroadcast");
    _ASSIGN_eventName(eXULCommandUpdate, "eXULCommandUpdate");

#  undef _ASSIGN_eventName

    default: {
      eventName.AssignLiteral("UNKNOWN: ");
      eventName.AppendInt(aGuiEvent->mMessage);
    } break;
  }

  return nsAutoString(eventName);
}
//////////////////////////////////////////////////////////////
//
// Code to deal with paint and event debug prefs.
//
//////////////////////////////////////////////////////////////
struct PrefPair {
  const char* name;
  bool value;
};

static PrefPair debug_PrefValues[] = {
    {"nglayout.debug.crossing_event_dumping", false},
    {"nglayout.debug.event_dumping", false},
    {"nglayout.debug.invalidate_dumping", false},
    {"nglayout.debug.motion_event_dumping", false},
    {"nglayout.debug.paint_dumping", false}};

//////////////////////////////////////////////////////////////
bool nsBaseWidget::debug_GetCachedBoolPref(const char* aPrefName) {
  NS_ASSERTION(nullptr != aPrefName, "cmon, pref name is null.");

  for (uint32_t i = 0; i < ArrayLength(debug_PrefValues); i++) {
    if (strcmp(debug_PrefValues[i].name, aPrefName) == 0) {
      return debug_PrefValues[i].value;
    }
  }

  return false;
}
//////////////////////////////////////////////////////////////
static void debug_SetCachedBoolPref(const char* aPrefName, bool aValue) {
  NS_ASSERTION(nullptr != aPrefName, "cmon, pref name is null.");

  for (uint32_t i = 0; i < ArrayLength(debug_PrefValues); i++) {
    if (strcmp(debug_PrefValues[i].name, aPrefName) == 0) {
      debug_PrefValues[i].value = aValue;

      return;
    }
  }

  NS_ASSERTION(false, "cmon, this code is not reached dude.");
}

//////////////////////////////////////////////////////////////
class Debug_PrefObserver final : public nsIObserver {
  ~Debug_PrefObserver() = default;

 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER
};

NS_IMPL_ISUPPORTS(Debug_PrefObserver, nsIObserver)

NS_IMETHODIMP
Debug_PrefObserver::Observe(nsISupports* subject, const char* topic,
                            const char16_t* data) {
  NS_ConvertUTF16toUTF8 prefName(data);

  bool value = Preferences::GetBool(prefName.get(), false);
  debug_SetCachedBoolPref(prefName.get(), value);
  return NS_OK;
}

//////////////////////////////////////////////////////////////
/* static */ void debug_RegisterPrefCallbacks() {
  static bool once = true;

  if (!once) {
    return;
  }

  once = false;

  nsCOMPtr<nsIObserver> obs(new Debug_PrefObserver());
  for (uint32_t i = 0; i < ArrayLength(debug_PrefValues); i++) {
    // Initialize the pref values
    debug_PrefValues[i].value =
        Preferences::GetBool(debug_PrefValues[i].name, false);

    if (obs) {
      // Register callbacks for when these change
      nsCString name;
      name.AssignLiteral(debug_PrefValues[i].name,
                         strlen(debug_PrefValues[i].name));
      Preferences::AddStrongObserver(obs, name);
    }
  }
}
//////////////////////////////////////////////////////////////
static int32_t _GetPrintCount() {
  static int32_t sCount = 0;

  return ++sCount;
}
//////////////////////////////////////////////////////////////
/* static */
void nsBaseWidget::debug_DumpEvent(FILE* aFileOut, nsIWidget* aWidget,
                                   WidgetGUIEvent* aGuiEvent,
                                   const char* aWidgetName, int32_t aWindowID) {
  if (aGuiEvent->mMessage == eMouseMove) {
    if (!debug_GetCachedBoolPref("nglayout.debug.motion_event_dumping")) return;
  }

  if (aGuiEvent->mMessage == eMouseEnterIntoWidget ||
      aGuiEvent->mMessage == eMouseExitFromWidget) {
    if (!debug_GetCachedBoolPref("nglayout.debug.crossing_event_dumping"))
      return;
  }

  if (!debug_GetCachedBoolPref("nglayout.debug.event_dumping")) return;

  NS_LossyConvertUTF16toASCII tempString(
      debug_GuiEventToString(aGuiEvent).get());

  fprintf(aFileOut, "%4d %-26s widget=%-8p name=%-12s id=0x%-6x refpt=%d,%d\n",
          _GetPrintCount(), tempString.get(), (void*)aWidget, aWidgetName,
          aWindowID, aGuiEvent->mRefPoint.x.value,
          aGuiEvent->mRefPoint.y.value);
}
//////////////////////////////////////////////////////////////
/* static */
void nsBaseWidget::debug_DumpPaintEvent(FILE* aFileOut, nsIWidget* aWidget,
                                        const nsIntRegion& aRegion,
                                        const char* aWidgetName,
                                        int32_t aWindowID) {
  NS_ASSERTION(nullptr != aFileOut, "cmon, null output FILE");
  NS_ASSERTION(nullptr != aWidget, "cmon, the widget is null");

  if (!debug_GetCachedBoolPref("nglayout.debug.paint_dumping")) return;

  nsIntRect rect = aRegion.GetBounds();
  fprintf(aFileOut,
          "%4d PAINT      widget=%p name=%-12s id=0x%-6x bounds-rect=%3d,%-3d "
          "%3d,%-3d",
          _GetPrintCount(), (void*)aWidget, aWidgetName, aWindowID, rect.X(),
          rect.Y(), rect.Width(), rect.Height());

  fprintf(aFileOut, "\n");
}
//////////////////////////////////////////////////////////////
/* static */
void nsBaseWidget::debug_DumpInvalidate(FILE* aFileOut, nsIWidget* aWidget,
                                        const LayoutDeviceIntRect* aRect,
                                        const char* aWidgetName,
                                        int32_t aWindowID) {
  if (!debug_GetCachedBoolPref("nglayout.debug.invalidate_dumping")) return;

  NS_ASSERTION(nullptr != aFileOut, "cmon, null output FILE");
  NS_ASSERTION(nullptr != aWidget, "cmon, the widget is null");

  fprintf(aFileOut, "%4d Invalidate widget=%p name=%-12s id=0x%-6x",
          _GetPrintCount(), (void*)aWidget, aWidgetName, aWindowID);

  if (aRect) {
    fprintf(aFileOut, " rect=%3d,%-3d %3d,%-3d", aRect->X(), aRect->Y(),
            aRect->Width(), aRect->Height());
  } else {
    fprintf(aFileOut, " rect=%-15s", "none");
  }

  fprintf(aFileOut, "\n");
}
//////////////////////////////////////////////////////////////

#endif  // DEBUG
