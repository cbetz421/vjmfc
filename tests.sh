videos=( video.1 video.2 video.3 video.5 video.6 video.7 video.h264 )
for video in "${videos[@]}"; do
    echo $video:
    ./vjmfc ../mymfc/$video
done
