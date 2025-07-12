/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

/* global docShell, sendAsyncMessage, addMessageListener, content */

function toggle(id) {
  var el = document.getElementById(id);
  if (el.getAttribute("collapsed")) {
    el.setAttribute("collapsed", false);
  } else {
    el.setAttribute("collapsed", true);
  }
}

function addCertErrorException(temporary) {
  document.addCertException(temporary);
  document.location.reload();
}

document
  .getElementById("technicalContentHeading")
  .addEventListener("click", () => {
    toggle("technicalContent");
  });

document
  .getElementById("expertContentHeading")
  .addEventListener("click", () => {
    toggle("expertContent");
  });

document
  .getElementById("temporaryExceptionButton")
  .addEventListener("click", () => {
    addCertErrorException(true);
  });

document
  .getElementById("permanentExceptionButton")
  .addEventListener("click", () => {
    addCertErrorException(false);
  });
