#include "TestFramework.h"
#include "Core/TakeFolderTracker.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

using namespace mma;

namespace {

struct ScratchFolder
{
    ScratchFolder()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() / ("sobstage-folder-tracker-" + std::to_string (stamp));
        std::filesystem::create_directories (root);
    }

    ~ScratchFolder()
    {
        std::error_code ignored;
        std::filesystem::remove_all (root, ignored);
    }

    std::filesystem::path root;
};

} // namespace

#if defined (__linux__) || defined (__APPLE__)

TEST_CASE (TakeFolderTracker_AnUnmovedFolderKeepsItsSpelling)
{
    ScratchFolder scratch;
    const auto take = scratch.root / "2026-10-06_1432_Kitchen";
    std::filesystem::create_directories (take);

    TakeFolderTracker tracker;
    REQUIRE (tracker.open (take.string()));
    REQUIRE (tracker.resolve() == take.string());
}

// The card copier's launch look tells a take it already follows from an
// earlier one by this, whatever the folder is called now. Mutation: the
// identity taken from the spelling rather than the folder.
TEST_CASE (TakeFolderTracker_TheFolderIsKnownByItselfNotItsName)
{
    ScratchFolder scratch;
    const auto take = scratch.root / "2026-10-06_1432_Kitchen";
    const auto other = scratch.root / "2026-10-06_1500_Hall";
    std::filesystem::create_directories (take);
    std::filesystem::create_directories (other);

    TakeFolderTracker held, hall;
    REQUIRE (held.open (take.string()));
    REQUIRE (hall.open (other.string()));
    REQUIRE_FALSE (held.fileIdentity().empty());
    REQUIRE (held.fileIdentity() != hall.fileIdentity());

    const auto renamed = scratch.root / "Kitchen show (keep)";
    std::filesystem::rename (take, renamed);

    TakeFolderTracker foundLater;
    REQUIRE (foundLater.open (renamed.string()));
    REQUIRE (foundLater.fileIdentity() == held.fileIdentity());

    TakeFolderTracker none;
    REQUIRE (none.fileIdentity().empty());
}

// The cameras' movies finish after the audio has closed its files. A take
// folder renamed in Finder in that gap has to be found by asking the folder.
TEST_CASE (TakeFolderTracker_FollowsAFolderRenamedAfterItWasOpened)
{
    ScratchFolder scratch;
    const auto take = scratch.root / "2026-10-06_1432_Kitchen";
    const auto renamed = scratch.root / "Kitchen show (keep)";
    std::filesystem::create_directories (take);

    TakeFolderTracker tracker;
    REQUIRE (tracker.open (take.string()));

    std::filesystem::rename (take, renamed);
    REQUIRE (std::filesystem::equivalent (std::filesystem::path (tracker.resolve()), renamed));

    // Moved again, into another folder.
    const auto archive = scratch.root / "Archive";
    std::filesystem::create_directories (archive);
    std::filesystem::rename (renamed, archive / "Kitchen show (keep)");
    REQUIRE (std::filesystem::equivalent (std::filesystem::path (tracker.resolve()),
                                          archive / "Kitchen show (keep)"));
}

// The combined video and the podcast copy run for minutes after the take has
// let go of its folder. Each gets a descriptor of its own, which still finds
// the folder after a rename once the take's tracker is closed.
TEST_CASE (TakeFolderTracker_ADuplicateFollowsTheFolderAfterTheOriginalCloses)
{
    ScratchFolder scratch;
    const auto take = scratch.root / "2026-10-11_2105_Late-set";
    const auto renamed = scratch.root / "Late set (keep)";
    std::filesystem::create_directories (take);

    TakeFolderTracker tracker;
    REQUIRE (tracker.open (take.string()));

    TakeFolderTracker later;
    REQUIRE (later.adopt (tracker.duplicateDescriptor(), take.string()));
    tracker.close();

    REQUIRE (later.resolve() == take.string());

    std::filesystem::rename (take, renamed);
    REQUIRE (std::filesystem::equivalent (std::filesystem::path (later.resolve()), renamed));
}

TEST_CASE (TakeFolderTracker_ADeletedFolderHasNoAnswer)
{
    ScratchFolder scratch;
    const auto take = scratch.root / "gone";
    std::filesystem::create_directories (take);

    TakeFolderTracker tracker;
    REQUIRE (tracker.open (take.string()));
    std::filesystem::remove (take);

    // Not the old path: nothing is there to find.
    REQUIRE (tracker.resolve().empty());
}

#endif

TEST_CASE (TakeFolderTracker_ClosedOrMissingAnswersNothing)
{
    TakeFolderTracker tracker;
    REQUIRE_FALSE (tracker.isOpen());
    REQUIRE (tracker.resolve().empty());
    REQUIRE (tracker.duplicateDescriptor() == -1);

    REQUIRE_FALSE (tracker.open ((std::filesystem::temp_directory_path() / "sobstage-no-such-take").string()));
    REQUIRE (tracker.resolve().empty());

    REQUIRE_FALSE (tracker.open (""));
}
