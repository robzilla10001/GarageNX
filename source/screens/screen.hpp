#pragma once
// source/screens/screen.hpp
// Abstract base for all GarageNX screens.
// Each screen owns its state. The main loop calls update() then draw() every frame.

#include <memory>
#include <string>

class Screen {
public:
    virtual ~Screen() = default;

    /// Called once when the screen becomes active (pushed onto the stack).
    virtual void on_enter() {}

    /// Called once when the screen is about to be replaced or popped.
    virtual void on_exit() {}

    /// Process input and update internal state.
    /// Return a non-null Screen to push a new screen onto the stack.
    /// Return nullptr to stay on this screen.
    /// Set pop = true to request this screen be popped (go back).
    virtual std::unique_ptr<Screen> update(bool& pop) {
        pop = false;
        return nullptr;
    }

    /// Draw the screen content area (between title bar and status bar).
    virtual void draw() = 0;

    /// Called when a modal this screen showed resolves (Confirmed/Cancelled).
    /// The main loop routes the result here on the frame the modal closes.
    /// Default: ignore.
    virtual void on_modal_result(int result) { (void)result; }

    /// Optional: return a list of button hints to show in the content area.
    /// Called after draw(). The main loop overlays them.
    virtual std::string hint_string() const { return ""; }
};
