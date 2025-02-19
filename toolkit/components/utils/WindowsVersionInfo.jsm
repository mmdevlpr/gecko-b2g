/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

var EXPORTED_SYMBOLS = ["WindowsVersionInfo"];

const { AppConstants } = ChromeUtils.import(
  "resource://gre/modules/AppConstants.jsm"
);
const lazy = {};
ChromeUtils.defineModuleGetter(
  lazy,
  "ctypes",
  "resource://gre/modules/ctypes.jsm"
);

const BYTE = lazy.ctypes.uint8_t;
const WORD = lazy.ctypes.uint16_t;
const DWORD = lazy.ctypes.uint32_t;
const WCHAR = lazy.ctypes.char16_t;
const BOOL = lazy.ctypes.int;

var WindowsVersionInfo = {
  UNKNOWN_VERSION_INFO: {
    servicePackMajor: null,
    servicePackMinor: null,
    buildNumber: null,
  },

  /**
   * Gets the service pack and build number on Windows platforms.
   *
   * @param opts {Object} keyword arguments
   * @param opts.throwOnError {boolean} Optional, defaults to true. If set to
   *    false will return an object with keys set to null instead of throwing an
   *    error. If set to true, errors will be thrown instead.
   * @throws If `throwOnError` is true and version information cannot be
   *    determined.
   * @return {object} An object containing keys `servicePackMajor`,
   *    `servicePackMinor`, and `buildNumber`. If `throwOnError` is false, these
   *    values may be null.
   */
  get({ throwOnError = true } = {}) {
    function throwOrUnknown(err) {
      if (throwOnError) {
        throw err;
      }
      Cu.reportError(err);
      return WindowsVersionInfo.UNKNOWN_VERSION_INFO;
    }

    if (AppConstants.platform !== "win") {
      return throwOrUnknown(
        WindowsVersionInfo.NotWindowsError(
          `Cannot get Windows version info on platform ${AppConstants.platform}`
        )
      );
    }

    // This structure is described at:
    // http://msdn.microsoft.com/en-us/library/ms724833%28v=vs.85%29.aspx
    const SZCSDVERSIONLENGTH = 128;
    const OSVERSIONINFOEXW = new lazy.ctypes.StructType("OSVERSIONINFOEXW", [
      { dwOSVersionInfoSize: DWORD },
      { dwMajorVersion: DWORD },
      { dwMinorVersion: DWORD },
      { dwBuildNumber: DWORD },
      { dwPlatformId: DWORD },
      { szCSDVersion: lazy.ctypes.ArrayType(WCHAR, SZCSDVERSIONLENGTH) },
      { wServicePackMajor: WORD },
      { wServicePackMinor: WORD },
      { wSuiteMask: WORD },
      { wProductType: BYTE },
      { wReserved: BYTE },
    ]);

    let kernel32;
    try {
      kernel32 = lazy.ctypes.open("kernel32");
    } catch (err) {
      return throwOrUnknown(
        new WindowsVersionInfo.CannotOpenKernelError(
          `Unable to open kernel32! ${err}`
        )
      );
    }

    try {
      let GetVersionEx = kernel32.declare(
        "GetVersionExW",
        lazy.ctypes.winapi_abi,
        BOOL,
        OSVERSIONINFOEXW.ptr
      );
      let winVer = OSVERSIONINFOEXW();
      winVer.dwOSVersionInfoSize = OSVERSIONINFOEXW.size;

      if (GetVersionEx(winVer.address()) === 0) {
        throw new WindowsVersionInfo.GetVersionExError(
          "Failure in GetVersionEx (returned 0)"
        );
      }

      return {
        servicePackMajor: winVer.wServicePackMajor,
        servicePackMinor: winVer.wServicePackMinor,
        buildNumber: winVer.dwBuildNumber,
      };
    } catch (err) {
      return throwOrUnknown(err);
    } finally {
      if (kernel32) {
        kernel32.close();
      }
    }
  },

  CannotOpenKernelError: class extends Error {},
  GetVersionExError: class extends Error {},
  NotWindowsError: class extends Error {},
};
