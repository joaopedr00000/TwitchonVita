Mozilla CA certificate bundle for Twitch IRC OAuth TLS verification.

Download the current PEM bundle from https://curl.se/ca/cacert.pem and save
it as assets/cacert.pem before running cmake locally. The GitHub CI test build
downloads this file over HTTPS automatically, checks certificate count and
packages it under ca/cacert.pem in the VPK.

Without this bundle the application does not send Twitch OAuth credentials
through IRC, and only anonymous read-only chat will be available.

Bundle licensed under MPL 2.0. The app's MIT license is unchanged.
