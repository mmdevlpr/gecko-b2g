/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

const RELATIVE_DIR = "toolkit/components/pdfjs/test/";
const TESTROOT = "http://example.com/browser/" + RELATIVE_DIR;

var MockFilePicker = SpecialPowers.MockFilePicker;
MockFilePicker.init(window);
MockFilePicker.returnValue = MockFilePicker.returnOK;

var tempDir;

async function promiseDownloadFinished(list) {
  return new Promise(resolve => {
    list.addView({
      onDownloadChanged(download) {
        download.launchWhenSucceeded = false;
        if (download.succeeded || download.error) {
          list.removeView(this);
          resolve(download);
        }
      },
    });
  });
}

function createPromiseForFilePicker() {
  return new Promise(resolve => {
    MockFilePicker.showCallback = fp => {
      let destFile = tempDir.clone();
      destFile.append(fp.defaultString);
      if (destFile.exists()) {
        destFile.remove(false);
      }
      MockFilePicker.setFiles([destFile]);
      MockFilePicker.filterIndex = 0; // kSaveAsType_Complete
      resolve();
    };
  });
}

add_setup(async function() {
  tempDir = createTemporarySaveDirectory();
  MockFilePicker.displayDirectory = tempDir;

  registerCleanupFunction(async function() {
    MockFilePicker.cleanup();
    await cleanupDownloads();
    tempDir.remove(true);
  });
});

async function closeDownloadsPanel() {
  if (DownloadsPanel.panel.state !== "closed") {
    let hiddenPromise = BrowserTestUtils.waitForEvent(
      DownloadsPanel.panel,
      "popuphidden"
    );
    DownloadsPanel.hidePanel();
    await hiddenPromise;
  }
  is(
    DownloadsPanel.panel.state,
    "closed",
    "Check that the download panel is closed"
  );
}

/**
 * Check clicking the download button saves the file and doesn't open a new viewer
 */
add_task(async function test_downloading_pdf_nonprivate_window() {
  const pdfUrl = TESTROOT + "file_pdfjs_test.pdf";

  await SpecialPowers.pushPrefEnv({
    set: [["browser.download.improvements_to_download_panel", true]],
  });

  await BrowserTestUtils.withNewTab(
    { gBrowser, url: "about:blank" },
    async function(browser) {
      await waitForPdfJS(browser, pdfUrl);

      const tabCount = gBrowser.tabs.length;
      info(`${tabCount} tabs are open at the start of the test`);

      let downloadList = await Downloads.getList(Downloads.PUBLIC);
      const initialDownloadCount = (await downloadList.getAll()).length;

      let filePickerShown = createPromiseForFilePicker();

      let downloadFinishedPromise = promiseDownloadFinished(downloadList);

      info("Clicking on the download button...");
      await SpecialPowers.spawn(browser, [], () => {
        content.document.querySelector("#download").click();
      });
      info("Waiting for a filename to be picked from the file picker");
      await filePickerShown;

      // check that resulted in a download being added to the list. The
      // download panel should not open.
      await downloadFinishedPromise;
      is(
        DownloadsPanel.panel.state,
        "closed",
        "Check the download panel state is 'closed'"
      );
      downloadList = await Downloads.getList(Downloads.PUBLIC);
      const allDownloads = await downloadList.getAll();
      let currentDownloadCount = allDownloads.length;
      is(
        currentDownloadCount,
        initialDownloadCount + 1,
        "A download was added when we clicked download"
      );

      const dl = allDownloads.find(dl => dl.source.originalUrl === pdfUrl);
      ok(!!dl, "The pdf download has the correct url in source.originalUrl");

      SpecialPowers.clipboardCopyString("");
      DownloadsCommon.copyDownloadLink(dl);
      const copiedUrl = SpecialPowers.getClipboardData("text/unicode");
      is(copiedUrl, pdfUrl, "The copied url must be the original one");

      is(
        gBrowser.tabs.length,
        tabCount,
        "No new tab was opened to view the downloaded PDF"
      );
    }
  );
});
