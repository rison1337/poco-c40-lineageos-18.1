package com.android.systemui.statusbar.phone;

import static org.junit.Assert.*;
import static org.mockito.Mockito.*;

import android.app.Fragment;
import android.content.Context;
import android.testing.DexmakerShareClassLoaderRule;
import androidx.test.InstrumentationRegistry;
import androidx.test.runner.AndroidJUnit4;
import com.android.systemui.Dependency;
import com.android.systemui.plugins.statusbar.StatusBarStateController;
import com.android.systemui.statusbar.CommandQueue;
import com.android.systemui.statusbar.policy.KeyguardStateController;
import com.android.systemui.statusbar.policy.NetworkController;
import com.android.systemui.tuner.TunerService;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.*;
import org.junit.runner.RunWith;

/** Complete production fragment; only Context attachment and external dependencies are supplied. */
@RunWith(AndroidJUnit4.class)
public final class C40StatusBarLifecycleTest {
    @Rule public final DexmakerShareClassLoaderRule shareClassLoader = new DexmakerShareClassLoaderRule();
    static final class Subject extends CollapsedStatusBarFragment {
        Context context;
        Subject(Context context) throws Exception {
            this.context = context;
            // No FragmentHostCallback in this isolated harness; loader lifecycle is unrelated.
            Field field = Fragment.class.getDeclaredField("mCheckedForLoaderManager");
            field.setAccessible(true); field.setBoolean(this, true);
        }
        @Override public Context getContext() { return context; }
    }
    final Set<TunerService.Tunable> listeners = new HashSet<>();
    final ArrayList<Subject> subjects = new ArrayList<>();
    TunerService tuner;
    boolean old;

    void main(Runnable action) throws Exception {
        AtomicReference<Throwable> failure = new AtomicReference<>();
        InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
            try { action.run(); } catch (Throwable t) { failure.set(t); }
        });
        if (failure.get() != null) throw new AssertionError("Fragment lifecycle failed", failure.get());
    }
    Subject create() throws Exception {
        Subject subject = new Subject(InstrumentationRegistry.getTargetContext());
        subjects.add(subject); main(() -> subject.onCreate(null)); return subject;
    }
    void reload() { for (TunerService.Tunable listener : new ArrayList<>(listeners))
        listener.onTuningChanged(StatusBarIconController.ICON_BLACKLIST, null); }
    @Before public void setup() throws Exception {
        old = InstrumentationRegistry.getArguments().getString("expect", "fixed").equals("old");
        tuner = mock(TunerService.class);
        doAnswer(a -> { TunerService.Tunable listener = (TunerService.Tunable) a.getArguments()[0];
            listeners.add(listener); listener.onTuningChanged(StatusBarIconController.ICON_BLACKLIST, null);
            return null;
        }).when(tuner).addTunable(any(TunerService.Tunable.class), anyString());
        doAnswer(a -> { listeners.remove(a.getArguments()[0]); return null; })
            .when(tuner).removeTunable(any(TunerService.Tunable.class));
        Dependency dependencies = new Dependency();
        Field cache = Dependency.class.getDeclaredField("mDependencies"); cache.setAccessible(true);
        java.util.Map<Object,Object> values = (java.util.Map<Object,Object>) cache.get(dependencies);
        values.put(TunerService.class, tuner);
        for (Class<?> type : new Class<?>[]{KeyguardStateController.class, NetworkController.class,
                StatusBarStateController.class, StatusBar.class, CommandQueue.class})
            values.put(type, mock(type));
        Field instance = Dependency.class.getDeclaredField("sDependency"); instance.setAccessible(true);
        instance.set(null, dependencies);
    }
    @After public void cleanup() { listeners.clear(); subjects.clear(); Dependency.clearDependencies(); }

    @Test public void activeFragmentReceivesDefaultSetting() throws Exception {
        Subject subject = create();
        assertTrue(listeners.contains(subject)); main(this::reload);
        verify(tuner).addTunable(subject, StatusBarIconController.ICON_BLACKLIST);
        verify(tuner, never()).removeTunable(subject);
    }
    @Test public void destroyedFragmentReleasesTunerRegistration() throws Exception {
        Subject subject = create(); main(subject::onDestroy);
        assertEquals(old, listeners.contains(subject));
        verify(tuner, old ? never() : times(1)).removeTunable(subject);
    }
    @Test public void userReloadAfterDetachReproducesOldCrashOnly() throws Exception {
        Subject subject = create();
        main(() -> {
            subject.onDestroy(); subject.context = null;
            boolean crashed = false;
            try { reload(); } catch (NullPointerException expected) { crashed = true; }
            assertEquals("Detached tuner callback must be removed", old, crashed);
        });
    }
    @Test public void replacementKeepsOnlyLiveFragmentRegistered() throws Exception {
        Subject first = create(); main(() -> { first.onDestroy(); first.context = null; });
        Subject replacement = create();
        assertTrue(listeners.contains(replacement)); assertEquals(old ? 2 : 1, listeners.size());
        if (!old) main(this::reload);
        main(replacement::onDestroy);
        assertEquals(old ? 2 : 0, listeners.size());
    }
}
