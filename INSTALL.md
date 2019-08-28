# How to Install Pacemaker

## Build Dependencies

| Fedora-based                 | Suse-based                   | Debian-based                   |
|:----------------------------:|:----------------------------:|:------------------------------:|
| automake 1.11 or later       | automake 1.11 or later       | automake 1.11 or later         |
| autoconf 2.64 or later       | autoconf 2.64 or later       | autoconf 2.64 or later         |
| bash                         | bash                         | bash                           |
| libtool                      | libtool                      | libtool                        |
| libtool-ltdl-devel           | libtool-ltdl-devel           | libltdl-dev                    |
| libuuid-devel                | libuuid-devel                | uuid-dev                       |
| pkgconfig                    | pkgconfig                    | pkg-config                     |
| python (or python-devel)     | python (or python-devel)     | python3 (or python3-dev)       |
| glib2-devel 2.16.0 or later  | glib2-devel 2.16.0 or later  | libglib2.0-dev 2.16.0 or later |
| libxml2-devel                | libxml2-devel                | libxml2-dev                    |
| libxslt-devel                | libxslt-devel                | libxslt-dev                    |
| bzip2-devel                  | bzip2-devel                  | libbz2-dev                     |
| libqb-devel                  | libqb-devel                  | libqb-dev                      |
|                              |                              | libcfg-dev                     |
|                              |                              | libcpg-dev                     |
|                              |                              | libcmap-dev                    |
|                              |                              | libquorum-dev                  |

## Cluster Stack Dependencies (Pick at least one)
* Corosync: corosynclib-devel
* (no other stacks are currently supported)

## Optional Build Dependencies
* gnutls-devel 2.1.7 or later (Pacemaker Remote and encrypted remote CIB admin)
* pam-devel (encrypted remote CIB admin)
* ncurses-devel (interactive crm_mon)
* systemd-devel (systemd support)
* dbus-devel (systemd/upstart resource support)
* cluster-glue-libs-devel (Linux-HA style fencing agents)
* asciidoc or asciidoctor (documentation)
* help2man (documentation)
* publican (documentation)
* inkscape (documentation)
* docbook-style-xsl (documentation)

## Optional testing dependencies
* valgrind (if running CTS valgrind tests)
* systemd-python (if using CTS on cluster nodes running systemd)
* rsync (if running CTS container tests)
* libvirt-daemon-driver-lxc (if running CTS container tests)
* libvirt-daemon-lxc (if running CTS container tests)
* libvirt-login-shell (if running CTS container tests)

## Source Control (GIT)

    git clone git://github.com/ClusterLabs/pacemaker.git

[See Github](https://github.com/ClusterLabs/pacemaker)

## Installing from source

    $ ./autogen.sh
    $ ./configure
    $ make
    $ sudo make install
