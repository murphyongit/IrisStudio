#pragma once

#include <memory>
#include <string>
#include <utility>

#include "App.hpp"

namespace Launcher
{
    // What the launcher knows and does: finding what the mod needs, setting it up and starting the game, the work
    // of each on a thread of its own; the UI thread asks for the home view to draw and passes the buttons on. The
    // settings are the app's, which the controller saves too when it finds the game.
    class Controller
    {
       public:
        Controller(const Options& options, Settings& settings);
        ~Controller();

        // takes in what the workers have done since the last frame
        void Update();
        Home BuildHome() const;
        // whether a worker is busy, so frames are drawn for its progress
        bool Busy() const;
        void Do(Action action);
        // after the settings changed (a folder picked in Settings)
        void SettingsChanged();
        // the window came to the front: what the user did meanwhile (installing Plutonium) is looked for again
        void Activated();
        std::string GameFolderText() const;
        bool CanRebuild() const;
        // the page of a release newer than this launcher, once the update check found one; empty until then
        std::string UpdateUrl() const;
        // whether the first look for what the mod needs is done (--shot waits for it)
        bool Detected() const;
        // a beta build's key as Settings shows it, and whether that is a problem (a key pasted that didn't work)
        std::pair<std::string, bool> BetaKeyRow() const;

       private:
        void RemoveFiles();

        struct State;
        std::unique_ptr<State> state;
    };
}  // namespace Launcher
