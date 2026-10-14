#! /bin/bash

# Copied from gstreamer/cerbero
# https://gitlab.freedesktop.org/gstreamer/cerbero/-/blob/2a0f5829aa1b56e8d36ef36f9675845345d54542/ci/docker_android_setup.sh

set -eux

export ANDROID_HOME=$1
export ANDROID_NDK_HOME=$2

mkdir -p /android/sources

curl -o /android/sources/android-ndk.zip https://dl.google.com/android/repository/android-ndk-r25c-linux.zip
unzip /android/sources/android-ndk.zip -d ${ANDROID_NDK_HOME}/
# remove the intermediate versioned directory
mv ${ANDROID_NDK_HOME}/*/* ${ANDROID_NDK_HOME}/

curl -o /android/sources/android-sdk-tools.zip https://dl.google.com/android/repository/commandlinetools-linux-9477386_latest.zip
unzip /android/sources/android-sdk-tools.zip -d ${ANDROID_HOME}/
mkdir -p ${ANDROID_HOME}/licenses

# Accept licenses. Values taken from:
# $ANDROID_HOME/tools/bin/sdkmanager --sdk_root=$ANDROID_HOME --licenses
# cd $ANDROID_HOME
# for f in licenses/*; do echo "echo \"$(cat $f | tr -d '\n')\" > \${ANDROID_HOME}/$f"; done
echo "601085b94cd77f0b54ff86406957099ebe79c4d6" > ${ANDROID_HOME}/licenses/android-googletv-license
echo "859f317696f67ef3d7f30a50a5560e7834b43903" > ${ANDROID_HOME}/licenses/android-sdk-arm-dbt-license
echo "24333f8a63b6825ea9c5514f83c2829b004d1fee" > ${ANDROID_HOME}/licenses/android-sdk-license
echo "84831b9409646a918e30573bab4c9c91346d8abd" > ${ANDROID_HOME}/licenses/android-sdk-preview-license
echo "33b6a2b64607f11b759f320ef9dff4ae5c47d97a" > ${ANDROID_HOME}/licenses/google-gdk-license
echo "e9acab5b5fbb560a72cfaecce8946896ff6aab9d" > ${ANDROID_HOME}/licenses/mips-android-sysimage-license

rm -rf /android/sources
