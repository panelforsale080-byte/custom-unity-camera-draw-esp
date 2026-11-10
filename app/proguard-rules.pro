-keep class com.custom.unitycamera.NativeBridge { *; }
-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}
-keep class com.custom.unitycamera.MainActivity { *; }
-keep class com.custom.unitycamera.OverlayService { *; }
-keep class com.custom.unitycamera.LineView { *; }
