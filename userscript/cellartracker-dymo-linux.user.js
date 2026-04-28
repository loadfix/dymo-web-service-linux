// ==UserScript==
// @name         CellarTracker DYMO for Linux
// @namespace    https://github.com/loadfix/dymo-web-service-linux
// @version      1.0.0
// @description  Patch dymo.connect.framework.js so CellarTracker prints to a local Linux DYMO web service emulator
// @match        https://www.cellartracker.com/*
// @run-at       document-end
// @grant        none
// ==/UserScript==

(function () {
  'use strict';

  const PRINTER_NAME = 'DYMO LabelWriter 450 Turbo';
  const MODEL_NAME = 'DYMO LabelWriter 450 Turbo';

  // The DYMO framework JS is loaded async. Poll until it's on the page, then patch once.
  const start = Date.now();
  const iv = setInterval(() => {
    if (window.dymo?.label?.framework && window.dymo.label.framework.chooseEnvironment) {
      clearInterval(iv);
      patch();
    } else if (Date.now() - start > 15000) {
      clearInterval(iv);
      console.warn('[dymo-linux] framework never loaded, giving up');
    }
  }, 100);

  function patch() {
    const fw = window.dymo.label.framework;
    const okEnv = {
      isBrowserSupported: true,
      isFrameworkInstalled: true,
      isWebServicePresent: true,
      errorDetails: '',
    };

    // Build the real DlsWebService wrapper while the framework's internal
    // chooseEnvironment still has the sync XHR path available. The wrapper
    // doesn't do sync XHR itself for these calls — it's created up front.
    let svc;
    try {
      svc = fw.chooseEnvironment(okEnv);
    } catch (e) {
      console.error('[dymo-linux] chooseEnvironment failed:', e);
      return;
    }
    fw.currentFramework = 2;

    // Prevent the framework from re-running its OS check.
    fw.checkEnvironment = function (cb) {
      if (cb) cb(okEnv);
      return okEnv;
    };

    // Printer enumeration: always return our local LabelWriter. The web
    // service will report the real printer state via /GetPrinters anyway,
    // but CellarTracker populates the dropdown from getLabelWriterPrinters().
    const mkPrinter = () =>
      new fw.LabelWriterPrinterInfo(PRINTER_NAME, MODEL_NAME, true, true, false);

    fw.getPrinters = () => [mkPrinter()];
    fw.getLabelWriterPrinters = () => [mkPrinter()];
    fw.getTapePrinters = () => [];
    fw.getDZPrinters = () => [];

    const asyncOne = (arr) => ({
      thenDo: (cb) => (cb(arr), { thenDo: () => {} }),
      then: (r) => (r(arr), { catch: () => {} }),
      addCallback: (cb) => cb(arr),
    });
    fw.getPrintersAsync = () => asyncOne([mkPrinter()]);
    fw.getLabelWriterPrintersAsync = () => asyncOne([mkPrinter()]);
    fw.getTapePrintersAsync = () => asyncOne([]);
    fw.getDZPrintersAsync = () => asyncOne([]);

    // Route print/render calls through the web service wrapper we built.
    fw.printLabel = (...a) => svc.printLabel(...a);
    fw.printLabel2 = (...a) => svc.printLabel2(...a);
    fw.renderLabel = (...a) => svc.renderLabel(...a);
    fw.printLabelAsync = (...a) => ({
      thenDo: (cb) => (cb(svc.printLabel(...a)), { thenDo: () => {} }),
    });

    console.log('[dymo-linux] framework patched — Linux web service enabled');
  }
})();
