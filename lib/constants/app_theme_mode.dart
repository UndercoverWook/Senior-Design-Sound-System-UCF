enum AppThemeMode { light, dark, rainbow }

extension AppThemeModeX on AppThemeMode {
  String get label {
    switch (this) {
      case AppThemeMode.light:
        return 'Light';
      case AppThemeMode.dark:
        return 'Dark';
      case AppThemeMode.rainbow:
        return 'Rainbow';
    }
  }

  String get description {
    switch (this) {
      case AppThemeMode.light:
        return 'Bright neutral interface';
      case AppThemeMode.dark:
        return 'Dark low-glare interface';
      case AppThemeMode.rainbow:
        return 'Animated multicolor interface';
    }
  }
}
