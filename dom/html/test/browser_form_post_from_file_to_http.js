/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim: set ft=javascript ts=2 et sw=2 tw=80: */

const TEST_HTTP_POST =
  "http://example.org/browser/dom/html/test/form_submit_server.sjs";

// Test for bug 1351358.
add_task(async function() {
  // Set prefs to ensure file content process, to allow linked web content in
  // file URI process and allow more that one file content process.
  await SpecialPowers.pushPrefEnv(
    {set: [["browser.tabs.remote.separateFileUriProcess", true],
           ["browser.tabs.remote.allowLinkedWebInFileUriProcess", true]]});

  // Create file URI and test data file paths.
  let testFile = getChromeDir(getResolvedURI(gTestPath));
  testFile.append("dummy_page.html");
  const fileUriString = Services.io.newFileURI(testFile).spec;
  let filePaths = [];
  testFile.leafName = "form_data_file.txt";
  filePaths.push(testFile.path);
  testFile.leafName = "form_data_file.bin";
  filePaths.push(testFile.path);

  // Open file:// page tab in which to run the test.
  await BrowserTestUtils.withNewTab(fileUriString, async function(fileBrowser) {

    // Create a form to post to server that writes posted data into body as JSON.
    let promiseLoad = BrowserTestUtils.browserLoaded(fileBrowser, false,
                                                     TEST_HTTP_POST);
    /* eslint-disable no-shadow */
    await ContentTask.spawn(fileBrowser, [TEST_HTTP_POST, filePaths],
                                         ([actionUri, filePaths]) => {
      Cu.importGlobalProperties(["File"]);

      let doc = content.document;
      let form = doc.body.appendChild(doc.createElement("form"));
      form.action = actionUri;
      form.method = "POST";
      form.enctype = "multipart/form-data";

      let inputText = form.appendChild(doc.createElement("input"));
      inputText.type = "text";
      inputText.name = "text";
      inputText.value = "posted";

      let inputCheckboxOn = form.appendChild(doc.createElement("input"));
      inputCheckboxOn.type = "checkbox";
      inputCheckboxOn.name = "checked";
      inputCheckboxOn.checked = true;

      let inputCheckboxOff = form.appendChild(doc.createElement("input"));
      inputCheckboxOff.type = "checkbox";
      inputCheckboxOff.name = "unchecked";
      inputCheckboxOff.checked = false;

      let inputFile = form.appendChild(doc.createElement("input"));
      inputFile.type = "file";
      inputFile.name = "file";
      let fileList = [];
      let promises = [];
      for (let path of filePaths) {
        promises.push(File.createFromFileName(path).then(file => {
          fileList.push(file);
        }));
      }

      Promise.all(promises).then(() => {
        inputFile.mozSetFileArray(fileList);
        form.submit();
      });
    });
    /* eslint-enable no-shadow */

    let href = await promiseLoad;
    is(href, TEST_HTTP_POST,
       "Check that the loaded page is the one to which we posted.");

    let binContentType;
    if (AppConstants.platform == "macosx") {
      binContentType = "application/macbinary";
    } else {
      binContentType = "application/octet-stream";
    }
    await ContentTask.spawn(fileBrowser, { binContentType }, (args) => {
      let data = JSON.parse(content.document.body.textContent);
      is(data[0].headers["Content-Disposition"], "form-data; name=\"text\"",
         "Check text input Content-Disposition");
      is(data[0].body, "posted", "Check text input body");

      is(data[1].headers["Content-Disposition"], "form-data; name=\"checked\"",
         "Check checkbox input Content-Disposition");
      is(data[1].body, "on", "Check checkbox input body");

      // Note that unchecked checkbox details are not sent.

      is(data[2].headers["Content-Disposition"],
         "form-data; name=\"file\"; filename=\"form_data_file.txt\"",
         "Check text file input Content-Disposition");
      is(data[2].headers["Content-Type"], "text/plain",
         "Check text file input Content-Type");
      is(data[2].body, "1234\n", "Check text file input body");

      is(data[3].headers["Content-Disposition"],
         "form-data; name=\"file\"; filename=\"form_data_file.bin\"",
         "Check binary file input Content-Disposition");
      is(data[3].headers["Content-Type"], args.binContentType,
         "Check binary file input Content-Type");
      is(data[3].body, "\u0001\u0002\u0003\u0004\n",
         "Check binary file input body");
    });
  });
});
