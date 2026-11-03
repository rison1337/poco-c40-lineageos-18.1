package com.android.systemui.statusbar.phone;

import static org.junit.Assert.*;
import static org.mockito.Mockito.*;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.testing.DexmakerShareClassLoaderRule;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import androidx.test.InstrumentationRegistry;
import androidx.test.runner.AndroidJUnit4;
import com.android.internal.widget.LockPatternUtils;
import com.android.keyguard.KeyguardHostView;
import com.android.keyguard.KeyguardUpdateMonitor;
import com.android.keyguard.ViewMediatorCallback;
import com.android.systemui.DejankUtils;
import com.android.systemui.keyguard.DismissCallbackRegistry;
import com.android.systemui.plugins.FalsingManager;
import com.android.systemui.statusbar.policy.KeyguardStateController;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.*;
import org.junit.runner.RunWith;

/** Actual production KeyguardBouncer. Only view inflation and service dependencies are mocked. */
@RunWith(AndroidJUnit4.class)
public final class C40BouncerTest {
    @Rule public final DexmakerShareClassLoaderRule shareClassLoader = new DexmakerShareClassLoaderRule();
    private static final class Subject extends KeyguardBouncer {
        final KeyguardHostView host;
        final ViewGroup root;
        Subject(Context context, FalsingManager falsing, BouncerExpansionCallback callback,
                KeyguardHostView host, ViewGroup root) {
            super(context, mock(ViewMediatorCallback.class), mock(LockPatternUtils.class),
                    new FrameLayout(context), mock(DismissCallbackRegistry.class), falsing,
                    callback, mock(KeyguardStateController.class), mock(KeyguardUpdateMonitor.class),
                    mock(KeyguardBypassController.class), new Handler(Looper.getMainLooper()));
            this.host=host;this.root=root;
        }
        @Override protected void inflateView() { mKeyguardView=host; mRoot=root; root.setVisibility(View.INVISIBLE); }
        void attach() { ensureView(); }
        void simulateRemovedView() { mRoot=null; }
    }
    private Subject subject;
    private FalsingManager falsing;
    private KeyguardHostView host;
    private ViewGroup root;
    private KeyguardBouncer.BouncerExpansionCallback callback;
    private boolean old;
    private int visibility;

    private void main(Runnable r) throws Exception {
        AtomicReference<Throwable> failure=new AtomicReference<>();
        InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {try {r.run();}catch(Throwable t){failure.set(t);}});
        if(failure.get()!=null)throw new AssertionError("Bouncer action failed",failure.get());
    }
    @Before public void setup() throws Exception {
        String expected=InstrumentationRegistry.getArguments().getString("expect", "fixed");
        assertTrue(expected.equals("old")||expected.equals("fixed"));old=expected.equals("old");
        falsing=mock(FalsingManager.class);host=mock(KeyguardHostView.class);root=mock(ViewGroup.class);
        callback=mock(KeyguardBouncer.BouncerExpansionCallback.class);visibility=View.INVISIBLE;
        when(root.getVisibility()).thenAnswer(a -> visibility);
        doAnswer(a -> {visibility=(Integer)a.getArguments()[0];return null;}).when(root).setVisibility(anyInt());
        when(host.getHeight()).thenReturn(500);
        main(() -> {
            DejankUtils.setImmediate(true);
            subject=new Subject(InstrumentationRegistry.getTargetContext(),falsing,callback,host,root);
        });
    }
    @After public void cleanup() throws Exception {main(() -> {subject.hide(false);DejankUtils.setImmediate(false);});}

    @Test public void preInflationExpansionDoesNotClaimActualBouncerShown() throws Exception {
        main(() -> subject.setExpansion(0f));
        verify(falsing,old?times(1):never()).onBouncerShown();
        verify(callback).onFullyShown();
        verify(host,never()).onResume();
        verify(root,never()).setVisibility(anyInt());
    }
    @Test public void preparedButInvisibleViewDoesNotClaimShown() throws Exception {
        main(() -> {subject.attach();subject.setExpansion(0f);});
        verify(falsing,old?times(1):never()).onBouncerShown();
        verify(host,old?times(1):never()).onResume();
    }
    @Test public void earlierZeroExpansionReplaysOnceWhenViewActuallyShows() throws Exception {
        main(() -> subject.setExpansion(0f));
        clearInvocations(falsing,host,callback);
        main(() -> subject.show(false));
        assertEquals(View.VISIBLE,visibility);
        verify(falsing,old?never():times(1)).onBouncerShown();
        verify(host).onResume();
        main(() -> subject.show(false));
        verify(falsing,old?never():times(1)).onBouncerShown();
    }
    @Test public void hideAndReshowAtSameExpansionStartsNewFalsingShownState() throws Exception {
        main(() -> subject.show(false));
        clearInvocations(falsing,host,callback);
        main(() -> {subject.hide(false);subject.show(false);});
        verify(falsing).onBouncerHidden();
        verify(falsing,old?never():times(1)).onBouncerShown();
        verify(host).onResume();assertEquals(View.VISIBLE,visibility);
    }
    @Test public void dismissedUnsecuredKeyguardDoesNotPretendChallengeShown() throws Exception {
        when(host.dismiss(anyInt())).thenReturn(true);
        main(() -> subject.show(false));
        verify(falsing,old?times(1):never()).onBouncerShown();
        assertEquals(View.INVISIBLE,visibility);
    }
    @Test public void normalShowAndExpansionKeepRequiredCallbacks() throws Exception {
        main(() -> subject.show(false));
        verify(falsing,times(1)).onBouncerShown();
        clearInvocations(falsing,host,callback);
        main(() -> {subject.setExpansion(1f);subject.show(false,false);subject.setExpansion(.5f);subject.setExpansion(0f);});
        verify(falsing,times(1)).onBouncerHidden();
        verify(falsing,times(1)).onBouncerShown();
        verify(callback).onFullyHidden();verify(callback).onFullyShown();
        verify(host).onResume();
        verify(host,atLeastOnce()).setAlpha(anyFloat());
        verify(host,atLeastOnce()).setTranslationY(anyFloat());
    }
    @Test public void removedRootCannotResumeStaleHostOrClaimShown() throws Exception {
        main(() -> {subject.attach();subject.simulateRemovedView();subject.setExpansion(0f);});
        verify(falsing,old?times(1):never()).onBouncerShown();
        verify(host,old?times(1):never()).onResume();
    }
    @Test public void resetThenShowReestablishesFalsingState() throws Exception {
        main(() -> subject.show(false));
        clearInvocations(falsing,host,callback);
        main(() -> {subject.reset();subject.show(false);});
        verify(falsing).onBouncerHidden();verify(falsing,old?never():times(1)).onBouncerShown();
        verify(host).onResume();assertEquals(View.VISIBLE,visibility);
    }
    @Test public void partialHideThenFullyShownPreservesRepeatedFalsingNotification() throws Exception {
        main(() -> subject.show(false));
        clearInvocations(falsing,host,callback);
        main(() -> {subject.setExpansion(.5f);subject.setExpansion(0f);});
        verify(falsing).onBouncerShown();
        verify(falsing,never()).onBouncerHidden();
        verify(host).onResume();verify(callback).onStartingToHide();verify(callback).onFullyShown();
    }
}
