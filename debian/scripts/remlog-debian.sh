#!/bin/bash
# Remmina - The GTK+ Remote Desktop Client
#
# remlog-debian.sh: Copyright (C) 2016 Matteo Nastasi
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor,
# Boston, MA  02110-1301, USA.
#
# In addition, as a special exception, the copyright holders give
# permission to link the code of portions of this program with the
# OpenSSL library under certain conditions as described in each
# individual source file, and distribute linked combinations
# including the two.
# You must obey the GNU General Public License in all respects
# for all of the code used other than OpenSSL. If you modify
# file(s) with this exception, you may extend this exception to your
# version of the file(s), but you are not obligated to do so. If you
# do not wish to do so, delete this exception statement from your
# version. If you delete this exception statement from all source
# files in the program, then also delete it here.

CACHE_FILE="$HOME/.remlog-debian.cache"
CONF_FILE="$HOME/.remlog-debian.conf"
TMP_FILE="/tmp/remlog-debian.$$.tmp"
usage () {
    local ret=$1

    echo "$0 <new CHANGELOG.md part> [> a_new_file.txt]"
    exit $ret
}

if [ $# -ne 1 ]; then
    usage 1
fi
if [ ! -f "$CONF_FILE" ]; then
    echo >&2
    echo "To be able to run this script you must have a github account and a github API token." >&2
    echo "Please create a file named '.remlog-debian.cf' in your home directory with:" >&2
    echo >&2
    echo "GH_USER=<your_github_user_account>" >&2
    echo "GH_TOKEN=<your_github_token>" >&2
    echo >&2
    exit 1
else
    source "$CONF_FILE"
fi

TOKEN="5afbd2119a8d25b93712e277cfdecc944615d4f9"
fin=$1
IFS='
'
cat "$1" | grep '^- ' | sed 's/\[\\#\([0-9]\+\)\](.*\1) \?//g' > "$TMP_FILE"
for i in $(cat "$TMP_FILE" | grep '^-' | grep '\[.*\](https://github.com/' | sed 's@.*\[\([^]]*\)](\([^)]*\)))@\1|\2@g' | sort | uniq); do
    userid="$(echo "$i" | cut -d '|' -f 1)"
    userurl="$(echo "$i" | cut -d '|' -f 2)"
    set -o pipefail
    username="$(grep "^${userid}|" "$CACHE_FILE" | cut -d '|' -f 2)"
    if [ $? -ne 0 ]; then
        echo "$userid not found" >&2
        username="$(curl -s -u "$GH_USER:$GH_TOKEN" -i https://api.github.com/users/$userid | grep '"name"' | sed 's/.*"name": "//g;s/".*//g;s@^ *@@g')"
        if [ $? -eq 0 ]; then
            echo "$userid|$username" >> "$CACHE_FILE"
        else
            echo "USER $userid NOT FOUND" >&2
        fi
    fi
    set +o pipefail

    if [ "$username" = "" ]; then
        username="$userid"
    fi
    echo "  [$username (https://github.com/$userid)]"
    cat "$TMP_FILE" | grep '^-' | grep "\[${userid}\](https://github.com/" | sed "s/^- /  * /g;s@(\[$userid\](https://github.com/$userid))@@g;s@(https://github.com/FreeRDP/Remmina.*@@g"
    echo
done

    cat "$TMP_FILE" | grep -v "\[.*\](https://github.com/" | sed "s/^- /  * /g;s@(https://github.com/FreeRDP/Remmina.*@@g"

rm "$TMP_FILE"
