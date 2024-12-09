# SegmentPuzzler

## Executables

Currently available: Windows (Installer; Portable version), MacOSX (M1; Intel), and as an AppImage (Ubuntu 22.04) [here](https://github.com/JoeGreiner/SegmentPuzzler/releases).

<details>
<summary>How to run an AppImage</summary>
    
* Download file, e.g. SegmentPuzzler.AppImage
* Terminal - set permissions to execute:
  ``` bash
  chmod u+x SegmentPuzzler.AppImage
  ```
* Terminal - execute: ./SegmentPuzzler.AppImage
  ``` bash
  ./SegmentPuzzler.AppImage
  ```
</details>

## Build from source

### Prerequisites

* CMake
* ITK 5 or 6 with ITKModuleReview enabled
* Qt 5
* Optional: OpenSSL (matching to the version Qt was built against). Only needed to download sample volumes.

<details>
<summary>Windows</summary>

1. **Clone the repository:**
    ```bash
    git clone https://github.com/JoeGreiner/SegmentPuzzler.git
    ```
2. **Open `CMakeLists.txt` with Qt Creator.**
3. **Configure the project.**
4. **Build the project.**
5. **Create a build directory and run CPack:**
    ```bash
    cd build
    cpack <build-dir>
    ```
    - This will generate both ZIP and NSIS installers.
6. **Run the installer to install SegmentPuzzler.**

</details>

<details>
<summary>Linux</summary>

1. **Clone the repository:**
    ```bash
    git clone https://github.com/JoeGreiner/SegmentPuzzler.git
    ```
2. **Create a build directory and navigate into it:**
    ```bash
    mkdir build && cd build
    ```
3. **Run CMake to configure the project:**
    ```bash
    cmake <source-dir>
    ```
4. **Build and install the project:**
    ```bash
    make install -j <number-of-cores>
    ```
5. **Deploy the application using the deployment script:**
    ```bash
    ./deploy/deploy_linux.sh
    ```
6. **Run the generated AppImage located in the build directory.**

</details>

<details>
<summary>macOS</summary>

1. **Clone the repository:**
    ```bash
    git clone https://github.com/JoeGreiner/SegmentPuzzler.git
    ```
2. **Create a build directory and navigate into it:**
    ```bash
    mkdir build && cd build
    ```
3. **Run CMake to configure the project:**
    ```bash
    cmake <source-dir>
    ```
4. **Build and install the project:**
    ```bash
    make install
    ```
5. **Package the application using CPack:**
    ```bash
    cpack -G DragNDrop <build-dir>
    ```
</details>


## Dependencies and Acknowledgments

A big thank you to the developers and contributors to:

* [ITK](https://github.com/InsightSoftwareConsortium/ITK): Used extensively for image handling and processing.
* [CMake](https://cmake.org/): Used to build the application.
* [Qt](https://www.qt.io/): Used for the GUI, licensed under the [LGPLv3](https://www.gnu.org/licenses/lgpl-3.0.en.html).
    - A copy of the license is included in the `licenses` directory.
    - The Qt framework is used unmodified in this project. Source code for Qt is available [here](https://download.qt.io/official_releases/).
    - Qt is dynamically linked in this application, allowing users to replace or update the Qt libraries. This can be done using the provided CMake build system, in compliance with LGPLv3 requirements.
* [qdarkstyle](https://github.com/ColinDuquesnoy/QDarkStyleSheet): Provides fantastic stylesheets for the application. Licensed under the MIT License, with a copy included in the `qdarkstyle` directory.
* [nitroshare](https://github.com/nitroshare/nitroshare-desktop): Inspiration for the `Windeployqt.cmake` script, which is licensed under the MIT License. A copy of the license can be found on top of the script.
* [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy), [linuxdeploy-plugin-qt](https://github.com/linuxdeploy/linuxdeploy-plugin-qt), and [AppImageKit](https://github.com/AppImage/AppImageKit): Tools used to create a Linux AppImage.

## License

SegmentPuzzler is licensed under the [MIT License](LICENSE).
