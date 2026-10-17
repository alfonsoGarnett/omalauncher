package dev.omarchy;

import java.lang.reflect.Method;
import android.content.ClipData;
import android.content.ClipDescription;
import android.net.Uri;

public class ClipImage {
    public static void main(String[] args) {
        if (args.length < 1) {
            System.err.println("Usage: ClipImage <content_uri>");
            return;
        }
        try {
            Uri uri = Uri.parse(args[0]);
            ClipDescription desc = new ClipDescription("Screenshot", new String[]{"image/png"});
            ClipData clip = new ClipData(desc, new ClipData.Item(uri));

            Class<?> sm = Class.forName("android.os.ServiceManager");
            Method getService = sm.getMethod("getService", String.class);
            Object binder = getService.invoke(null, "clipboard");
            Class<?> stub = Class.forName("android.content.IClipboard$Stub");
            Method asInterface = stub.getMethod("asInterface", Class.forName("android.os.IBinder"));
            Object iclip = asInterface.invoke(null, binder);

            Method setPrimaryClip = iclip.getClass().getMethod("setPrimaryClip", ClipData.class, String.class, String.class, int.class, int.class);
            setPrimaryClip.invoke(iclip, clip, "com.android.shell", null, 0, 0);
            System.out.println("Omarchy: Android image clipboard set successfully to " + uri);
        } catch (Exception e) {
            e.printStackTrace();
        }
    }
}
