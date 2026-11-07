package org.libsdl.app;

/** Typed text for the game (SDL text input, i.e. WM_CHAR): SDLInputConnection is package-private in SDL. */
public final class SporeText {
    private SporeText() {}

    public static void type(String text) {
        SDLInputConnection.nativeCommitText(text, 1);
    }
}
