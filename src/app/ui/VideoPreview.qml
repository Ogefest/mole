import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtMultimedia

// The file playing, a pause button, and where you are.
//
// Nothing else, on purpose: a preview is for recognising a file rather than for
// working on one, so there is no volume slider to argue about, no playlist and no
// stepping frame by frame. See MOLE-37, and MOLE-223 for why it plays by itself.
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
        audioOutput: AudioOutput {
            // Bound rather than assigned. The answer lives in the controller
            // because that is where it is remembered -- one setting for every
            // video, kept across files and across restarts -- and this is the one
            // place it is applied.
            muted: view.controller ? view.controller.muted : false
        }

        // Starts on its own, as soon as there is something to start. Opening a
        // preview of a video is asking to see it move: the answer to *what is in
        // this file* is its first few seconds, and a still frame with a button on
        // it makes somebody ask for that twice.
        //
        // This was the other way round until MOLE-223, on the argument that F3
        // walks a folder with the arrows and a viewer making noise as the cursor
        // passes over a file is the wrong default. The argument is real and was
        // overruled: see docs/adr/0053-a-video-preview-plays-itself.md, which also
        // says what to do if the walking turns out to be the louder complaint.
        //
        // Only on `LoadedMedia`, which is the transition into having a file. A
        // clip that reaches its end goes to `EndOfMedia` and stays there, so
        // nothing here starts it over.
        onMediaStatusChanged: {
            if (mediaStatus === MediaPlayer.LoadedMedia && playbackState === MediaPlayer.StoppedState)
                play()
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
        color: App.colour.bad
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
            color: App.colour.textMuted
            font.pixelSize: App.smallTextSize
        }

        // The sound, off or on. It exists because the viewer plays by itself since
        // MOLE-223: a preview that makes noise on arrival and offers no way to stop
        // it is worse than one that waited to be told to play. A speaker rather
        // than a slider -- mute is the question actually being asked, and a value
        // to drag would be a value to argue about in a viewer whose whole point is
        // that it is small. See MOLE-225.
        Button {
            objectName: "videoMuteButton"
            text: view.controller && view.controller.muted ? "\u{1F507}" : "\u{1F50A}"
            flat: true
            focusPolicy: Qt.NoFocus
            // Icon-only, so it takes the window's hit-target floor and the text
            // size inside it, like the bookmark and close-tab buttons.
            font.pixelSize: App.textSize
            implicitWidth: App.minimumTarget
            implicitHeight: App.minimumTarget
            ToolTip.text: view.controller && view.controller.muted ? "Turn the sound on" : "Turn the sound off"
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: if (view.controller) view.controller.setMuted(!view.controller.muted)
        }
    }
}
