Plex New Transcoder Unofficial Source Repository README
=======================================================

This is the source pulled from [Plex](http://www.plex.tv/) for the Plex Media Server (closed source application).

This repository sat dormant with a very old version of teh transcoder source and I decided to fix that, so I automated the
detection of the transcoder and wrote a script that downloads it automatically and updates this repository which I have 
scheduled to run nightly (see below).

The initial version (since this update) is from December 2017.  There are two transcoders referenced in their documentation
but only one of the links works; this appears to be the "New Transcoder" (which they now indicate is used for transcoding
whereas the other one is used for media scanning).

Refresh and Update
------------------

I've configured a cron job on a remote server to execute the refresh-repo.zsh script, daily.  It will make a commit every time it
sees a new version (regardless of whether or not that version has new ffmpeg code), however, it will only update ffmpeg code when
the code is updated

Unfortunately, there are caveats involved in all of this.  Plex doesn't provide a very easy way to automate discovery of the 
location of the ffmpeg sources.  The only method I could find was to grep the LICENSE file in the Plex archive, which is what the
`refresh-repo.zsh` script currently does.  In addition, at least as of the May 2018, one of the links doesn't work (I'll follow
up with them on that), so only one of the archives is present.  If they substantially change the names of the files, the script
might fail to parse/distinguish between the scanner and the transcoder ffmpeg code.

That ... and I wrote the script in about 10 minutes.  The code quality is pretty awful and it's `zsh` (only tested with 5.5.1).
Something else, entirely, could go wrong.

Consequently, if the script update fails due to the cron job or my server going down, you can simply clone this repo and run
the refresh-repo.zsh (and if it isn't obvious, that's a Zsh script, not a `bash` script).  It will blow up trying to push
to my repository (or you can modify that line), but outside of that -- assuming the problem is my server -- it should at least
get you the latest version (and use up a few hundred megs of disk space in the process of downloading all of the pieces/parts
required to get there).
