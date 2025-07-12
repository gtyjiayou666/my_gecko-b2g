/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ContextMenuUtils: "resource://gre/modules/ContextMenuUtils.sys.mjs",
  ScreenshotUtils: "resource://gre/modules/ScreenshotUtils.sys.mjs",
});

export class WebViewForContentChild extends JSWindowActorChild {
  log(...args) {
    dump("WebViewForContentChild: ");
    for (let a of args) {
      dump(a + " ");
    }
    dump("\n");
  }

  actorCreated() {
    lazy.ContextMenuUtils.init(this);
  }

  getBackgroundColor(browser, event) {
    let eventName = event.detail.id;
    let content = browser.contentWindow;
    const win = browser.ownerGlobal;

    let backgroundcolor = "transparent";
    try {
      backgroundcolor = content
        .getComputedStyle(content.document.body)
        .getPropertyValue("background-color");

      browser?.dispatchEvent(
        new win.CustomEvent(
          eventName,
          Cu.cloneInto(
            {
              bubbles: true,
              detail: {
                success: true,
                result: backgroundcolor,
              },
            },
            win
          )
        )
      );
    } catch (e) {
      browser?.dispatchEvent(
        new win.CustomEvent(eventName, {
          success: false,
        })
      );
    }
  }

  async getScreenshot(browser, event) {
    let content = browser.contentWindow;
    let eventName = event.detail.id;
    const win = browser.ownerGlobal;
    if (!content) {
      browser.dispatchEvent(
        new win.CustomEvent(eventName, {
          success: false,
        })
      );
      return;
    }

    let { maxWidth, maxHeight, mimeType } = event.detail;
    let detail = { success: false };
    try {
      let blob = await lazy.ScreenshotUtils.getScreenshot(
        content,
        maxWidth,
        maxHeight,
        mimeType
      );
      detail = Cu.cloneInto(
        {
          detail: {
            success: true,
            result: blob,
          },
        },
        win
      );
    } catch (e) {}
    browser.dispatchEvent(new win.CustomEvent(eventName, detail));
  }

  handleEvent(event) {
    let bc = this.browsingContext;

    if (event.type == "contextmenu") {
      // Handle the case that contextmenu is fired in a iframe. Other events
      // fired in a iframe are ignored.
      while (
        bc &&
        bc.embedderElement &&
        HTMLIFrameElement.isInstance(bc.embedderElement)
      ) {
        bc = bc.parent;
      }
    }

    const browser = bc.embedderElement;

    if (
      !browser ||
      !XULFrameElement.isInstance(browser) ||
      browser.isRemoteBrowser ||
      Services.appinfo.processType == Ci.nsIXULRuntime.PROCESS_TYPE_DEFAULT
    ) {
      // We only handle the window which is one of in-process <web-view>'s children.
      return;
    }
    this.log(
      `${this.contentWindow.document.URL} handleEvent: (${event.type}) browser=(${browser})`
    );

    switch (event.type) {
      case "DOMTitleChanged": {
        this.fireTitleChanged(event);
        break;
      }
      case "webview-getbackgroundcolor": {
        this.getBackgroundColor(browser, event);
        break;
      }
      case "webview-getscreenshot": {
        this.getScreenshot(browser, event);
        break;
      }
      case "DOMMetaAdded":
      case "DOMMetaChanged":
      case "DOMMetaRemoved": {
        this.fireMetaChanged(event);
        break;
      }
      case "DOMLinkAdded": {
        this.handleLinkAdded(event);
        break;
      }
      case "contextmenu": {
        this.handleContextMenu(event, browser);
        break;
      }
      case "webview-fire-ctx-callback": {
        lazy.ContextMenuUtils.handleContextMenuCallback(
          this,
          this.contentWindow,
          event.detail.menuitem
        );
        break;
      }
      case "scroll": {
        this.handleScroll(event);
        break;
      }
      case "MozScrolledAreaChanged": {
        this.handleScrollAreaChanged(event);
        break;
      }
    }
  }

  receiveMessage(message) {}

  fireTitleChanged(event) {
    const browser = this.browsingContext.embedderElement;
    const win = browser.ownerGlobal;
    // The actor child fires pagetitlechanged to the browser element in the
    // webview element and then the webview element listen the event and fires
    // titlechange with this.browser.contentTitle.
    browser?.dispatchEvent(
      new win.CustomEvent(
        "pagetitlechanged",
        Cu.cloneInto(
          {
            bubbles: true,
            detail: {},
          },
          win
        )
      )
    );
  }

  fireMetaChanged(event) {
    let target = event.target;
    const browser = this.browsingContext.embedderElement;
    const win = browser.ownerGlobal;
    browser?.dispatchEvent(
      new win.CustomEvent(
        "metachange",
        Cu.cloneInto(
          {
            bubbles: true,
            detail: {
              name: target.name,
              content: target.content,
              type: event.type.replace("DOMMeta", "").toLowerCase(),
            },
          },
          win
        )
      )
    );
  }

  handleLinkAdded(event) {
    let iconchangeHandler = this.iconChangedHandler.bind(this);
    let handlers = {
      icon: iconchangeHandler,
      "apple-touch-icon": iconchangeHandler,
      "apple-touch-icon-precomposed": iconchangeHandler,
      search: this.openSearchHandler.bind(this),
      manifest: this.manifestChangedHandler.bind(this),
    };

    this.log(`Got linkAdded: (${event.target.href}) ${event.target.rel}`);
    event.target.rel.split(" ").forEach(function (x) {
      let token = x.toLowerCase();
      if (handlers[token]) {
        handlers[token](event);
      }
    }, this);
  }

  handleScroll(event) {
    let doc = event.target;
    const browser = this.browsingContext.embedderElement;
    if (doc != this.contentWindow.document || event.defaultPrevented) {
      return;
    }
    const win = browser.ownerGlobal;
    browser?.dispatchEvent(
      new win.CustomEvent(
        "scroll",
        Cu.cloneInto(
          {
            detail: {
              top: doc.ownerGlobal.scrollY,
              left: doc.ownerGlobal.scrollX,
            },
          },
          win
        )
      )
    );
  }

  handleScrollAreaChanged(event) {
    let doc = event.target;
    const browser = this.browsingContext.embedderElement;
    if (doc != this.contentWindow.document || event.defaultPrevented) {
      return;
    }
    const win = browser.ownerGlobal;
    browser?.dispatchEvent(
      new win.CustomEvent(
        "scrollareachanged",
        Cu.cloneInto(
          {
            detail: {
              height: event.height,
              width: event.width,
            },
          },
          win
        )
      )
    );
  }

  maybeCopyAttribute(src, target, attribute) {
    if (src.getAttribute(attribute)) {
      target[attribute] = src.getAttribute(attribute);
    }
  }

  iconChangedHandler(event) {
    let target = event.target;
    this.log(`Got iconchanged: (${target.href})`);

    let icon = { href: target.href };
    this.maybeCopyAttribute(target, icon, "sizes");
    this.maybeCopyAttribute(target, icon, "rel");

    // The event target is the web-view element of a content window.
    // Dispatch the event to the related frame element.
    const browser = this.browsingContext.embedderElement;
    const win = browser.ownerGlobal;
    browser?.dispatchEvent(new win.CustomEvent("iconchange", { detail: icon }));
  }

  openSearchHandler(event) {
    let target = event.target;
    this.log(`Got opensearch: (${target.href})`);

    if (target.type !== "application/opensearchdescription+xml") {
      return;
    }

    // The event target is the web-view element of a content window.
    // Dispatch the event to the related frame element.
    const browser = this.browsingContext.embedderElement;
    const win = browser.ownerGlobal;
    browser?.dispatchEvent(
      new win.CustomEvent("opensearch", {
        detail: {
          title: target.title,
          href: target.href,
        },
      })
    );
  }

  manifestChangedHandler(event) {
    let target = event.target;
    this.log(`Got manifestchanged: (${target.href})`);

    // The event target is the web-view element of a content window.
    // Dispatch the event to the related frame element.
    const browser = this.browsingContext.embedderElement;
    const win = browser.ownerGlobal;
    browser?.dispatchEvent(
      new win.CustomEvent("manifestchange", {
        detail: {
          href: target.href,
        },
      })
    );
  }

  handleContextMenu(event, browser) {
    if (event.defaultPrevented) {
      return;
    }
    let menuData = lazy.ContextMenuUtils.generateMenu(
      this,
      this.contentWindow,
      event
    );
    menuData.nested = true;
    const win = browser.ownerGlobal;

    let ev = new win.CustomEvent("contextmenu", { detail: menuData });
    if (menuData.contextmenu) {
      let self = this.contentWindow;
      Cu.exportFunction(
        function (id) {
          let evt = new self.CustomEvent(
            "webview-fire-ctx-callback",
            Cu.cloneInto(
              {
                bubbles: true,
                cancelable: true,
                detail: {
                  menuitem: id,
                },
              },
              self
            )
          );
          self.dispatchEvent(evt);
        },
        ev.detail,
        { defineAs: "contextMenuItemSelected" }
      );
    }

    this.log(`dispatch ${event.type} ${JSON.stringify(ev.detail)}\n`);
    if (!browser.dispatchEvent(ev)) {
      // We call preventDefault() on our contextmenu event if the embedder
      // called preventDefault() on /its/ contextmenu event to stop firing a
      // click or long tap.
      event.preventDefault();
    } else {
      lazy.ContextMenuUtils.cancel(this);
    }
  }
}
