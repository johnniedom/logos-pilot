# Logos Design System Reference

Source: ~/dev/logos/logos-design-system/

Consumer pattern: add as flake input, copy lib/Logos/ into your UI project.

## Theme: Dark Only (currently)

No light theme exists. Only DarkTheme.qml.

## Colors (DarkTheme semantic tokens)

- background: #171717 (gray900)
- backgroundSecondary: #262626 (gray850)
- backgroundTertiary: #1C1C1C (gray875)
- backgroundElevated: #0E121B (gray950)
- surface: #343434 (gray320)
- text: #FFFFFF (white)
- textSecondary: #A4A4A4 (gray400)
- textTertiary: #969696 (gray500)
- textMuted: #5C5C5C (gray700)
- primary: #ED7B58 (orange300)
- primaryHover: #F55702 (orange500)
- primaryPressed: #F57A02 (orange600)
- success: #49F563 (green500)
- error: #FB3748 (red500)
- warning: #FEBC2E (yellow400)
- info: #4A90E2 (blue400)
- border: #434343 (gray300)
- borderSubtle: #333333 (gray330)
- borderStrong: #515151 (gray350)

## Typography

- Font: Public Sans (Regular 400, Medium 500, Bold 700)
- mainTitleText: 256px (splash)
- pageTitleText: 36px
- titleText: 30px
- panelTitleText: 24px
- subtitleText: 16px
- primaryText: 14px (default body)
- secondaryText: 12px (caption)

## Spacing

- tiny: 4px
- small: 8px
- medium: 12px
- large: 16px
- xlarge: 20px
- xxlarge: 40px

## Radius

- radiusSmall: 4px
- radiusMedium: 6px
- radiusLarge: 8px
- radiusXlarge: 16px
- radiusPill: 999px

## Available Components (36)

LogosButton, LogosIconButton, LogosTextField, LogosTextArea, LogosSearchBar,
LogosCheckbox, LogosRadioButton, LogosSwitch, LogosSlider, LogosSpinBox,
LogosComboBox, LogosProgressBar, LogosSpinner, LogosBadge, LogosToolTip,
LogosDialog, LogosDrawer, LogosFrame, LogosGroupBox, LogosMenu,
LogosMenuItem, LogosMenuSeparator, LogosPaginator, LogosScrollBar,
LogosScrollView, LogosStackView, LogosTabBar, LogosTabButton, LogosTable,
LogosTableColumn, LogosText, LogosToolBar, LogosToolButton,
LogosToolSeparator, LogosItemDelegate, LogosIcons

## Usage in QML

```qml
import Logos.Theme
import Logos.Controls

Rectangle {
    color: Theme.palette.background

    LogosButton {
        text: "Approve"
    }

    LogosTextField {
        placeholderText: "Message your agent..."
    }

    Text {
        text: "Balance: 12,847 LEZ"
        color: Theme.palette.text
        font.family: Theme.typography.publicSans
        font.pixelSize: Theme.typography.primaryText
    }
}
```

## For Phase 7 (Basecamp UI)

Use ONLY Theme.palette.* tokens and Logos.Controls components. No custom colors. No custom styled components. The UI must look native inside Basecamp alongside other modules.
