package com.android.systemui.volume;

import static org.junit.Assert.*;
import static org.mockito.Mockito.*;

import android.content.Context;
import android.media.AudioManager;
import android.os.PowerManager;
import android.os.Looper;
import android.testing.DexmakerShareClassLoaderRule;
import androidx.test.InstrumentationRegistry;
import androidx.test.runner.AndroidJUnit4;
import com.android.systemui.broadcast.BroadcastDispatcher;
import com.android.systemui.keyguard.WakefulnessLifecycle;
import com.android.systemui.statusbar.phone.StatusBar;
import com.android.systemui.util.RingerModeLiveData;
import com.android.systemui.util.RingerModeTracker;
import com.android.settingslib.volume.MediaSessions;
import dagger.Lazy;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.*;
import org.junit.runner.RunWith;

/** Runs the real controller on a worker while the StatusBar provider rejects access. */
@RunWith(AndroidJUnit4.class)
public class C40VolumeControllerWorkerTest {
    @Rule public final DexmakerShareClassLoaderRule shareClassLoader = new DexmakerShareClassLoaderRule();
    private BroadcastDispatcher dispatcher;
    private RingerModeTracker ringer;
    private WakefulnessLifecycle wakefulness;
    private PowerManager power;
    private VolumeDialogControllerImpl.C callbacks;
    private Subject subject;

    @Before public void setup() {
        dispatcher=mock(BroadcastDispatcher.class);
        ringer=mock(RingerModeTracker.class);
        RingerModeLiveData external=mock(RingerModeLiveData.class),internal=mock(RingerModeLiveData.class);
        when(external.getValue()).thenReturn(-1); when(internal.getValue()).thenReturn(-1);
        when(ringer.getRingerMode()).thenReturn(external); when(ringer.getRingerModeInternal()).thenReturn(internal);
        wakefulness=mock(WakefulnessLifecycle.class); power=mock(PowerManager.class);
        callbacks=mock(VolumeDialogControllerImpl.C.class);
    }

    private void create(boolean hasStatusBar) {
        InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
            Lazy<StatusBar> forbidden=() -> { throw new AssertionError("StatusBar initialized on volume worker"); };
            subject=new Subject(InstrumentationRegistry.getTargetContext(),dispatcher,
                    hasStatusBar?Optional.of(forbidden):Optional.empty(),ringer,wakefulness,power);
            subject.mCallbacks=callbacks; subject.setEnableDialogs(true,true);
        });
    }

    private void volume(int flags) throws Exception {
        AtomicReference<Throwable> failure=new AtomicReference<>();
        Thread worker=new Thread(() -> {
            try { subject.onVolumeChangedW(AudioManager.STREAM_VOICE_CALL,flags); }
            catch(Throwable t) { failure.set(t); }
        },"early-volume-regression");
        worker.start(); worker.join(3000);
        assertFalse("Worker blocked on UI initialization",worker.isAlive());
        if(failure.get()!=null) throw new AssertionError("Volume worker failed",failure.get());
    }

    @Test public void bootBroadcastAndEarlyKeyNeverResolveStatusBar() throws Exception {
        create(true);
        when(wakefulness.getWakefulness()).thenReturn(WakefulnessLifecycle.WAKEFULNESS_AWAKE);
        when(power.isInteractive()).thenReturn(true);
        volume(0);
        verify(power,never()).isInteractive();
        verify(callbacks,never()).onShowRequested(Events.SHOW_REASON_VOLUME_CHANGED);
        volume(AudioManager.FLAG_SHOW_UI);
        verify(callbacks,times(1)).onShowRequested(Events.SHOW_REASON_VOLUME_CHANGED);
    }

    @Test public void sleepAndInteractiveGatesStillApply() throws Exception {
        create(true);
        when(power.isInteractive()).thenReturn(true);
        when(wakefulness.getWakefulness()).thenReturn(WakefulnessLifecycle.WAKEFULNESS_ASLEEP);
        volume(AudioManager.FLAG_SHOW_UI);
        when(wakefulness.getWakefulness()).thenReturn(WakefulnessLifecycle.WAKEFULNESS_GOING_TO_SLEEP);
        volume(AudioManager.FLAG_SHOW_UI);
        when(wakefulness.getWakefulness()).thenReturn(WakefulnessLifecycle.WAKEFULNESS_AWAKE);
        when(power.isInteractive()).thenReturn(false);
        volume(AudioManager.FLAG_SHOW_UI);
        verify(callbacks,never()).onShowRequested(Events.SHOW_REASON_VOLUME_CHANGED);
        when(power.isInteractive()).thenReturn(true);
        when(wakefulness.getWakefulness()).thenReturn(WakefulnessLifecycle.WAKEFULNESS_WAKING);
        volume(AudioManager.FLAG_SHOW_UI);
        verify(callbacks,times(1)).onShowRequested(Events.SHOW_REASON_VOLUME_CHANGED);
    }

    @Test public void noStatusBarRetainsFlagsOnlyBehavior() throws Exception {
        create(false);
        volume(0); volume(AudioManager.FLAG_SHOW_UI);
        verify(power,never()).isInteractive();
        verify(callbacks,times(1)).onShowRequested(Events.SHOW_REASON_VOLUME_CHANGED);
        subject.setEnableDialogs(false,false);
        volume(AudioManager.FLAG_SHOW_UI);
        verify(callbacks,times(1)).onShowRequested(Events.SHOW_REASON_VOLUME_CHANGED);
    }

    @After public void cleanup() {
        if(subject!=null) InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> subject.destroy());
    }

    private static final class Subject extends VolumeDialogControllerImpl {
        Subject(Context c,BroadcastDispatcher d,Optional<Lazy<StatusBar>> s,RingerModeTracker r,
                WakefulnessLifecycle w,PowerManager p) { super(c,d,s,r,w,p); }
        @Override protected int getAudioManagerStreamVolume(int stream) { return 4; }
        @Override protected int getAudioManagerStreamMaxVolume(int stream) { return 10; }
        @Override protected int getAudioManagerStreamMinVolume(int stream) { return 0; }
        @Override protected MediaSessions createMediaSessions(Context context,Looper looper,
                MediaSessions.Callbacks callbacks) {
            // Media-session registration is unrelated to the volume worker's UI
            // gate. Avoid privileged Binder side effects in this isolated target.
            return mock(MediaSessions.class);
        }
    }
}
