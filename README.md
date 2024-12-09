# SegmentPuzzler

Tool to manipulate (merge, unmerge, regenerate, paint, select, ...) and proofread supervoxel-based segmentations. Under construction, more to come.

<video src="https://github.com/user-attachments/assets/44012456-4d42-4e00-93a5-8bacb68d5d3b" autoplay muted loop playsinline style="width:100%; max-width:800px; display:block; margin:0 auto;">
  Your browser does not support the video tag.
</video>




## Executables

<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 448 512"><!--!Font Awesome Free 6.7.1 by @fontawesome - https://fontawesome.com License - https://fontawesome.com/license/free Copyright 2024 Fonticons, Inc.--><path d="M0 93.7l183.6-25.3v177.4H0V93.7zm0 324.6l183.6 25.3V268.4H0v149.9zm203.8 28L448 480V268.4H203.8v177.9zm0-380.6v180.1H448V32L203.8 65.7z"/></svg>



Currently available: Windows (Installer; Portable version), MacOSX (M1; Intel), and as an AppImage (Ubuntu 22.04; CentOS9) [here](https://github.com/JoeGreiner/SegmentPuzzler/releases). AppImages should be compatible across different Linux distributions, as long as the libstdc++ isn't super old. Please reach out if you encounter issues.

<details>
<summary>How to run an AppImage</summary>
    
* Download file, e.g. SegmentPuzzler.AppImage
* Terminal - set permissions to execute:
  ``` bash
  chmod u+x SegmentPuzzler.AppImage
  ```
* Terminal - execute AppImage:
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
