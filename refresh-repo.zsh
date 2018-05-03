#!/bin/zsh
# Fugly script for refreshing repo from Plex
# Plex appears to only indicate the download location of their patched FFMPEG in their license file
#   so this script checks for new versions of plex, downloads the BSD version (since it's a simple
#   compressed tar file), extracts it, parses (poorly) the license file, downloads the two archives
#   (one of which is a dead link, currently) and, if it's newer than the last one it downloaded, 
#   puts the files in the correct directory, commits the changes, and pushes them to the GitHub
#   mirror.
# Please accept my sincere apologies for the quality of this script, the fact that I wrote it in
#   zsh, rather than a more portable language, or the fact that I didn't even bother to validate
#   it in versions of zsh before 5.5.1.  I don't have nearly enough time to do all of the things
#   I'd like to do to the level of quality I'd like to do them to and this is *very* low on my
#   priority list.  I spent all of 10 minutes on it and the code quality certainly reflects that.

# This runs as a cron job on my server and all output is logged, rather than printed.  The
# die function ensures that the "archive" folder is wiped out if it exists since the most likely
# place that it will fail is in those parts of code and I didn't want to litter them with clean-up
typeset -gr SCRIPT_PATH="${0:A:h}"
typeset -gr WORK_PATH="${SCRIPT_PATH}/run"
typeset -gr ARCHIVE_PATH="${WORK_PATH}/archive"
typeset -gr OLD_TC_EX_PATH="${ARCHIVE_PATH}/ffmpegs/PlexTranscoder"
typeset -gr NEW_TC_EX_PATH="${ARCHIVE_PATH}/ffmpegs/NewPlexTranscoder"
typeset -gr CODE_PATH="${SCRIPT_PATH}/plex-ffmpeg-source"
typeset -gr OLD_TC_PATH="${CODE_PATH}/PlexTranscoder"
typeset -gr NEW_TC_PATH="${CODE_PATH}/NewPlexTranscoder"

die() {
    echo "FATAL: $1"
    rm -rf "${WORK_PATH}/archive" > /dev/null 2>&1
    exit 1
}

cd "${SCRIPT_PATH}"
[[ ! -d ".git" ]] && {
    git init
    git remote add origin git@github:Diagonactic/plex-new-transcoder.git
    echo "run/" > .gitignore
    echo "*.tar.bz2" >> .gitignore
    echo "*.tar.xz" >> .gitignore
    echo "*.txz" >> .gitignore
    git add .gitignore
    git commit -m "Automated - initial .gitignore push"
    git push --set-upstream origin master
}

# Creates a directory, including its entire tree, if it doesn't exist.
make_dir() {
    if [[ ! -d "$1" ]]; then
        mkdir -p "$1" || die "Failed to create directory $1"
    fi
}

typeset -g api_version=''
typeset -g pms_url=''
query_pms_update_service() {
	() {
	    echo "Querying PMS update service at $1"
	    [[ -n "$1" ]] || die "Update request is invalid"
        api_version="`cat "$1" | jq --raw-output '.computer.FreeBSD.version'`"
        [[ -n "$api_version" ]] || die "Failed to get API version from update service"
        pms_url="`cat "$1" | jq --raw-output '.computer.FreeBSD.releases[].url'`"
        [[ -n "$pms_url" ]] || die "Failed to get archive URL from update service"
    } =(curl "https://plex.tv/api/downloads/1.json")
}

pms_requires_update() {
    query_pms_update_service
    if [[ ! -f "${SCRIPT_PATH}/last.version" ]]; then 
        echo "Last downloaded version is not known; PMS requires updating"
        return 0; 
    fi
    local last_version="$(cat "${SCRIPT_PATH}/last.version")"
    echo "API Reports version $api_version; Last version downloaded was $last_version"
    if [[ "$last_version" == "$api_version" ]]; then 
        echo "Last version, $last_version, matched API version."
        return 1; 
    fi
    echo "PMS requires update"
    return 0
}

# ...and that global won't be set until the function below it is called
download_and_extract() {
    local url="$1" target_path="$2"
    [[ -n "$url" ]] || die "Invalid parameters for download_and_extract: '$1'"
    [[ -z "$target_path" ]] || {
        make_dir "$target_path"
        cd "$target_path" || die "Couldn't change directory to $target_path for download_and_extract"
    }
    { curl "$1" | tar xj } || {
        echo "Couldn't download or extract data from '$1'"
        return 1
    }
    return 0
}


get_ffmpeg_archives_from_server() {
    cd "${WORK_PATH}/archive/pms"
    local license_file="$(find | grep 'Resources/LICENSE')"
    [[ -n "$license_file" ]] || die "Failed to locate LICENSE file for parsing"
    
    local ffmpeg_url=''
    local -i ffct=0 updated=0
    update_code() {
        local url="$1" temp_path="$2" repo_path="$3" compare_file="$4"
        echo "Downloading $url to '$temp_path'"
        () {
            local archive_file="$1"
            cp "$1" "$compare_file.downloaded"
            echo "Download completed to $1"
            [[ -n "$1" ]] || {
                echo "Failed to download ffmpeg archive from $url"
                return 1
            }
            
            if [[ ! -f "$compare_file" ]] || ! sha256sum "${SCRIPT_PATH}/$compare_file" "$archive_file"; then
                echo "Either compare_file didn't exist or it was different - extracting and updating code"
                make_dir "$temp_path"
                cd "$temp_path"
                echo "Extracting ${archive_file}"
                tar xf "${archive_file}" || {
                    echo "Failed to extract ${archive_file}"
                    return 1
                }
                typeset -a extracted_files; extracted_files=( )
                extracted_files="`print -l "${temp_path}/"*`"                
                [[ ! -d "$repo_path" ]] || rm -rf "$repo_path" > /dev/null 2>&1
                if (( ${#extracted_files[@]} == 1 )); then
                    echo "Moving code from ${extracted_files[1]} to ${repo_path}"
                    mv "${extracted_files[1]}" "${repo_path}"
                    cp "$archive_file" "$compare_file"
                    echo "Move completed"
                else
                    echo "Moving code from ${extracted_files[1]} to ${repo_path}"
                    make_dir "${repo_path}"
                    mv "$temp_path"/.* "${repo_path}"/
                    mv "$temp_path"/* "${repo_path}"/
                    cp "$archive_file" "$compare_file"
                    echo "Move completed"
                fi
            else
                echo "The ffmpeg code was the same as the existing archive"
                return 1
            fi
            return 0
        } =(curl --location "$url")
        return $?
    }
    cat "$license_file" | grep -e 'http[s]*://[^/]*/.*$' -o | grep 'ffmpeg' | while read ffmpeg_url; do
        if [[ $ffmpeg_url == "plex-ffmpeg-"* ]]; then
            update_code "${ffmpeg_url}" "${NEW_TC_EX_PATH}" "${NEW_TC_PATH}" "${SCRIPT_PATH}/PlexNewTranscoder.tar.xz" && updated=1
        elif [[ $ffmpeg_url == "Plex"* ]]; then
            update_code "${ffmpeg_url}" "${OLD_TC_EX_PATH}" "${OLD_TC_PATH}" "${SCRIPT_PATH}/PlexTranscoder.tar.bz2" && updated=1
        elif (( ffct == 0 )); then
            update_code "${ffmpeg_url}" "${NEW_TC_EX_PATH}" "${NEW_TC_PATH}" "${SCRIPT_PATH}/PlexNewTranscoder.tar.xz" && updated=1
        elif (( ffct == 1 )); then
            update_code "${ffmpeg_url}" "${OLD_TC_EX_PATH}" "${OLD_TC_PATH}" "${SCRIPT_PATH}/PlexTranscoder.tar.bz2" && updated=1
        else
            echo "WARNING: Unidentified ffmpeg URL: $ffmpeg_url"
        fi
        (( ffct++ ))
    done
    (( $updated == 1 )) && return 0 || return 1
}


make_dir "$WORK_PATH"
make_dir "$CODE_PATH"

cd "$WORK_PATH"
typeset commit_msg=''
if pms_requires_update; then
    download_and_extract "$pms_url" "${WORK_PATH}/archive/pms"
    if ! get_ffmpeg_archives_from_server; then
        echo -n "$api_version" > "${SCRIPT_PATH}/latest.version"
        commit_msg="Automated Update - No FFMPEG Changes Detected - "
    else
        echo -n "$api_version" > "${SCRIPT_PATH}/latest.version"
        commit_msg="Automated Update - "
    fi
    commit_msg+="Synchronized with version $api_version on `date -u --rfc-3339=seconds`"
    cd "${SCRIPT_PATH}"
    #git add .
    #git commit -m "$commit_msg"
    #git push
fi
rm -rf ${WORK_PATH} > /dev/null 2>&1
