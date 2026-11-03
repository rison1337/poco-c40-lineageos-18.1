package com.android.systemui.statusbar;

import static org.junit.Assert.*;
import static org.mockito.Mockito.*;

import android.app.WallpaperManager;
import android.os.Binder;
import android.os.IBinder;
import android.testing.DexmakerShareClassLoaderRule;
import android.view.Choreographer;
import android.view.View;
import androidx.test.InstrumentationRegistry;
import androidx.test.runner.AndroidJUnit4;
import com.android.systemui.dump.DumpManager;
import com.android.systemui.plugins.statusbar.StatusBarStateController;
import com.android.systemui.statusbar.phone.BiometricUnlockController;
import com.android.systemui.statusbar.phone.DozeParameters;
import com.android.systemui.statusbar.phone.NotificationShadeWindowController;
import com.android.systemui.statusbar.policy.KeyguardStateController;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.*;
import org.junit.runner.RunWith;
import org.mockito.ArgumentCaptor;

/** Real production controller; mocked window/animation services never touch live SystemUI. */
@RunWith(AndroidJUnit4.class)
public class C40ShadeDepthTest {
    @Rule public final DexmakerShareClassLoaderRule shareClassLoader = new DexmakerShareClassLoaderRule();
    private NotificationShadeDepthController subject;
    private WallpaperManager wallpaper;
    private NotificationShadeWindowController shadeWindow;
    private Choreographer choreographer;
    private View root;
    private NotificationShadeDepthController.DepthListener listener;
    private boolean old;

    private void onMain(Runnable action) throws Exception {
        AtomicReference<Throwable> failure = new AtomicReference<>();
        InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
            try { action.run(); } catch (Throwable t) { failure.set(t); }
        });
        if (failure.get() != null) throw new AssertionError("Controller action failed", failure.get());
    }

    @Before public void setup() throws Exception {
        String expected = InstrumentationRegistry.getArguments().getString("expect", "fixed");
        assertTrue("Explicit old/fixed expectation required", expected.equals("old") || expected.equals("fixed"));
        old = expected.equals("old");
        root = mock(View.class);
        wallpaper = mock(WallpaperManager.class);
        choreographer = mock(Choreographer.class);
        shadeWindow = mock(NotificationShadeWindowController.class);
        listener = mock(NotificationShadeDepthController.DepthListener.class);
        BlurUtils blur = mock(BlurUtils.class);
        when(blur.getMinBlurRadius()).thenReturn(0);
        when(blur.getMaxBlurRadius()).thenReturn(150);
        when(blur.ratioOfBlurRadius(anyInt())).thenAnswer(a -> ((Integer)a.getArguments()[0])/150f);
        onMain(() -> {
            subject = new NotificationShadeDepthController(mock(StatusBarStateController.class),
                    blur, mock(BiometricUnlockController.class), mock(KeyguardStateController.class),
                    choreographer, wallpaper, shadeWindow, mock(DozeParameters.class), new DumpManager());
            subject.setRoot(root);
            subject.getShadeSpring().setRadius(150);
            subject.getShadeAnimation().setRadius(150);
            subject.addListener(listener);
        });
        clearInvocations(choreographer);
    }

    private void frame() throws Exception {
        onMain(() -> subject.getUpdateBlurCallback().doFrame(0));
    }

    private View.OnAttachStateChangeListener attachListener(View view) {
        ArgumentCaptor<View.OnAttachStateChangeListener> capture = ArgumentCaptor.forClass(View.OnAttachStateChangeListener.class);
        verify(view).addOnAttachStateChangeListener(capture.capture());
        return capture.getValue();
    }

    @Test public void detachedFrameKeepsLocalStateWithoutNullTokenTransaction() throws Exception {
        frame();
        verify(wallpaper, old ? times(1) : never()).setWallpaperZoomOut(isNull(), eq(1f));
        verify(shadeWindow).setBackgroundBlurRadius(150);
        verify(listener).onWallpaperZoomOutChanged(1f);
    }

    @Test public void validTokenStillReceivesNonzeroWallpaperZoom() throws Exception {
        IBinder token = new Binder();
        when(root.getWindowToken()).thenReturn(token);
        frame();
        verify(wallpaper).setWallpaperZoomOut(same(token), eq(1f));
        verify(shadeWindow).setBackgroundBlurRadius(150);
        verify(listener).onWallpaperZoomOutChanged(1f);
    }

    @Test public void attachReplaysDepthAndDeduplicatesScheduledFrame() throws Exception {
        frame();
        clearInvocations(choreographer, wallpaper);
        if (old) {
            verify(root, never()).addOnAttachStateChangeListener(any());
            return;
        }
        View.OnAttachStateChangeListener attached = attachListener(root);
        IBinder token = new Binder();
        when(root.getWindowToken()).thenReturn(token);
        when(root.isAttachedToWindow()).thenReturn(true);
        onMain(() -> { attached.onViewAttachedToWindow(root); attached.onViewAttachedToWindow(root); });
        verify(choreographer, times(1)).postFrameCallback(same(subject.getUpdateBlurCallback()));
        frame();
        verify(wallpaper).setWallpaperZoomOut(same(token), eq(1f));
    }

    @Test public void replacedRootRemovesOldListenerAndRejectsItsLateCallback() throws Exception {
        View next = mock(View.class);
        View.OnAttachStateChangeListener attached = old ? null : attachListener(root);
        onMain(() -> subject.setRoot(next));
        if (old) {
            verify(root, never()).removeOnAttachStateChangeListener(any());
            verify(next, never()).addOnAttachStateChangeListener(any());
            return;
        }
        verify(root).removeOnAttachStateChangeListener(same(attached));
        attachListener(next);
        clearInvocations(choreographer);
        onMain(() -> attached.onViewAttachedToWindow(root));
        verify(choreographer, never()).postFrameCallback(any());
    }

    @Test public void alreadyAttachedRootGetsInitialDepthFrame() throws Exception {
        View next = mock(View.class);
        when(next.isAttachedToWindow()).thenReturn(true);
        when(next.getWindowToken()).thenReturn(new Binder());
        onMain(() -> subject.setRoot(next));
        verify(choreographer, old ? never() : times(1)).postFrameCallback(same(subject.getUpdateBlurCallback()));
        frame();
        verify(wallpaper).setWallpaperZoomOut(same(next.getWindowToken()), eq(1f));
    }
}
