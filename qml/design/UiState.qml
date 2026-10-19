pragma Singleton
pragma ComponentBehavior: Bound
import QtQuick
import Swiftify

// what the window shows, as opposed to what the player and library hold:
// the immersive player, and requests for dialogs and the catchphrase pill
// that any view may raise.
QtObject {
    id: root

    property bool immersiveOpen: false

    /// music plays and the window shows it: what the squiggles and shapes
    /// that turn wait for. the main window sets it.
    property bool spinning: false
    /// radians a second. it eases up when music starts and down when it
    /// stops; a spring here overshot and made the turning lurch
    property real spinSpeed: spinning && !Appearance.reduceMotion ? 0.9 : 0
    Behavior on spinSpeed {
        NumberAnimation { duration: 900; easing.type: Easing.InOutSine }
    }
    /// one turn shared by everything that spins, so they move together. it
    /// grows without end; whatever reads it wraps it. it ticks at 30 hz
    /// (AmbientClock), so what only turns slowly does not have the window
    /// drawn at the display's full rate for as long as music plays
    readonly property real spin: spinClock.value
    /// one clock, which the shapes that turn follow themselves
    /// (MaterialShape.spinRate) rather than through a binding apiece
    readonly property AmbientClock spinClock: App.spinClock
    readonly property Binding spinDrive: Binding {
        target: root.spinClock
        property: "speed"
        value: root.spinSpeed
    }
    readonly property string catchphrase: "i mean i guess bro"

    /// a clicked cover flying to the hero of the page it opens (HeroFlight).
    /// the click arms it with where the cover was; the hero that appears
    /// next claims it as its target, unless it has gone stale first. going
    /// back, the hero's cover flies home to the card it came from.
    property rect heroFrom
    property real heroBaseSize: 0
    property url heroSource
    property bool heroCircular: false
    property bool heroArmed: false
    /// the cover that was clicked, kept with the page it is on for the way back
    property ArtworkImage heroCover: null
    /// the corners it leaves with and lands with, as they look on screen
    property real heroFromRadius: Appearance.sizes.artworkRadius
    property real heroToRadius: Appearance.sizes.artworkRadius
    /// the hero's artwork box the cover is flying to, while it flies
    property Item heroTarget: null
    /// the hero's own cover has arrived, so the flying one may hand over
    property bool heroReady: false
    readonly property Timer heroExpiry: Timer {
        interval: 600
        onTriggered: root.heroArmed = false
    }

    /// called as a cover is clicked, before the page it opens is built
    function launchHero(cover: ArtworkImage, source: url, circular: bool) {
        if (Appearance.performanceMode || cover.width <= 0 || source.toString().length === 0) {
            return
        }
        heroFrom = cover.mapToItem(null, 0, 0, cover.width, cover.height)
        heroBaseSize = cover.width
        heroSource = source
        heroCircular = circular
        heroCover = cover
        heroFromRadius = cover.radius
        heroToRadius = Appearance.sizes.artworkRadius
        heroArmed = true
        heroExpiry.restart()
    }

    /// going back: the hero's cover, from where it showed (in the window),
    /// flies to the card it came from, which waits hidden until it lands
    function launchReturn(from: rect, cover: ArtworkImage, source: url, circular: bool) {
        if (Appearance.performanceMode) {
            return
        }
        heroArmed = false
        heroExpiry.stop()
        heroFrom = from
        heroBaseSize = cover.width
        heroSource = source
        heroCircular = circular
        heroFromRadius = Appearance.sizes.artworkRadius
        heroToRadius = cover.radius
        // the card's cover is drawn already
        heroReady = true
        heroTarget = cover
    }

    /// what a hero does as it is built: takes the cover in flight, if any
    function claimHero(box: Item): bool {
        if (!heroArmed) {
            return false
        }
        heroArmed = false
        heroExpiry.stop()
        heroReady = false
        heroTarget = box
        return true
    }

    signal toastRequested(string message)
    /// a little burst of shapes from an item (ShapeBurst, hosted by the
    /// main window): hearts, play, pin or playlist
    signal burstRequested(Item item, string kind)

    function burst(item: Item, kind: string) {
        if (item !== null && !Appearance.performanceMode) {
            burstRequested(item, kind)
        }
    }
    signal createPlaylistRequested()
    /// a new playlist of music files on this computer, starting with
    /// `first` when that is a song (or null)
    signal createLocalPlaylistRequested(var first)
    /// a music file of one's own, to edit what its tags say
    signal editLocalTrackRequested(song song)
    signal editPlaylistRequested()
    signal deletePlaylistRequested(playlist playlist)
    signal settingsRequested()
    /// the settings, open at a page and scrolled to a section of it, by the
    /// section's objectName
    signal settingsSectionRequested(int page, string section)

    function showCatchphrase(message: string) {
        toastRequested(message.length > 0 ? message : catchphrase)
    }

    /// what the + in the sidebar and the new-playlist tile do: an old library
    /// login has to be upgraded before it may create anything.
    function beginCreatePlaylist() {
        if (Session.libraryNeedsAuthorizationUpgrade) {
            Session.beginLibraryLogin()
        } else {
            createPlaylistRequested()
        }
    }

    /// leaving the immersive player first, as going to an album or artist does
    function goToAlbum(song: song) {
        immersiveOpen = false
        Library.goToAlbum(song)
    }

    function goToArtist(song: song) {
        immersiveOpen = false
        Library.goToArtist(song)
    }

    /// connecting for the first time (SetupView) is under way: it shows
    /// until playback is connected as well as the library, not only the
    /// library. it begins only once the saved logins have been read, so a
    /// launch that restores the library before playback never shows it, nor
    /// does playback dropping later on
    property bool setupUnderway: false
    /// the setup shows rather than the library
    readonly property bool needsSetup: !Session.libraryConnected || (setupUnderway && !Session.connected)

    function followSetup() {
        if (!Session.evaluatingStoredLogin && !Session.libraryConnected) {
            setupUnderway = true
        } else if (Session.libraryConnected && Session.connected) {
            setupUnderway = false
        }
    }

    readonly property Connections setupFollower: Connections {
        target: Session
        function onLibraryConnectedChanged() { root.followSetup() }
        function onConnectedChanged() { root.followSetup() }
        function onEvaluatingStoredLoginChanged() { root.followSetup() }
    }
    Component.onCompleted: followSetup()
}
