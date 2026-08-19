import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtMultimedia

// The first frame, a play button, and where you are.
//
// Nothing else, on purpose: a preview is for recognising a file rather than for
// working on one, so there is no volume slider to argue about, no playlist and no
// stepping frame by frame. See MOLE-37.
Item {
    id: view
    property var controller: null

    function clockText(milliseconds) {
        if (milliseconds <= 0)
            return "0:00"
        var total = Math.floor(milliseconds / 1000)
        var minutes = Math.floor(total / 60)
        var seconds = total % 60
        return minutes + ":" + (seconds < 10 ? "0" : "") + seconds
    }

    MediaPlayer {
        id: player
        objectName: "videoPlayer"
        source: controller ? controller.source : ""
        videoOutput: output
        audioOutput: AudioOutput {}

        // Never autoPlay. F3 walks a folder with the arrows, and a viewer that
        // starts making noise as the cursor passes over a file is the wrong
        // default -- so the file is paused at its first frame, which is what puts
        // a picture on screen without the video running.
        onMediaStatusChanged: {
            if (mediaStatus === MediaPlayer.LoadedMedia && playbackState === MediaPlayer.StoppedState)
                pause()
        }

        // A container this build can demux may still hold a stream it has no
        // decoder for, and there is no way to know before trying. So it says so,
        // where every other viewer says it, instead of leaving a black frame that
        // reads as a broken file.
        onErrorOccurred: function(playerError, errorString) {
            if (controller)
                controller.reportPlaybackFailure(errorString)
        }
    }

    // Stops rather than waiting to be collected. The tab drops a viewer as soon as
    // the file changes, so this is belt and braces -- but a player left running
    // behind the next preview is exactly the fault worth being sure about.
    Component.onDestruction: player.stop()

    VideoOutput {
        id: output
        objectName: "videoFrame"
        anchors.fill: parent
        anchors.margins: 8
        anchors.bottomMargin: controls.height + 12
        fillMode: VideoOutput.PreserveAspectFit
    }

    BusyIndicator {
        anchors.centerIn: parent
        running: (controller && controller.loading)
                 || player.mediaStatus === MediaPlayer.LoadingMedia
        visible: running
    }

    Label {
        anchors.centerIn: parent
        objectName: "videoErrorText"
        width: Math.min(parent.width - 40, 420)
        visible: controller && controller.errorText.length > 0
        text: controller ? controller.errorText : ""
        color: Material.color(Material.Red)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
    }

    RowLayout {
        id: controls
        objectName: "videoControls"
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 8
        spacing: 8
        visible: controller && controller.errorText.length === 0

        Button {
            objectName: "videoPlayButton"
            text: player.playbackState === MediaPlayer.PlayingState ? "Pause" : "Play"
            flat: true
            focusPolicy: Qt.NoFocus
            enabled: player.mediaStatus !== MediaPlayer.NoMedia
            onClicked: player.playbackState === MediaPlayer.PlayingState ? player.pause() : player.play()
        }

        Slider {
            objectName: "videoPosition"
            Layout.fillWidth: true
            from: 0
            to: Math.max(1, player.duration)
            // Not while it is being dragged: the position keeps arriving from the
            // player and would fight the hand on the handle.
            value: pressed ? value : player.position
            enabled: player.seekable
            onMoved: player.position = value
        }

        Label {
            objectName: "videoPositionText"
            text: view.clockText(player.position) + " / " + view.clockText(player.duration)
            color: "#8b93a7"
            font.pixelSize: App.smallTextSize
        }
    }
}
