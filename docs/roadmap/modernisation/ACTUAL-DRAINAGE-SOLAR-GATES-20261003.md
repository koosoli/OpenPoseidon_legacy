# Actual daylight drainage gates

The installed bd590703 build corrects the daylight sun-travel sign. The natural
rain-water runner now requires the actual available sun, negative daytime travel
Y, zero night effect, and the shared solar equation in both stopped and drained
receipts. Sunny exposure must exceed0.5 with clear sky; the cloud control must
have overcast between0.5 and0.7 and reduced positive exposure below0.25. Both
controls now use the same ordinary noon date command. Existing no-input mass,
native-source, ownership and normal-exit gates remain unchanged.

Pure checks reject missing sun, the old positive sign, zero solar contribution,
night and cloudy sunny controls. PowerShell Math.Min/Max bounds use floating
point literals explicitly: integer-first overload resolution otherwise rounds
the fractional source before evaluating the oracle.

This is a stronger runtime oracle, not new drainage coefficients or proof that
visible pools dry satisfactorily. Actual installed sunshine/cloud runs remain
pending. Across-process random rain histories may differ, so rate attribution
still needs source/timestep controls rather than a raw volume comparison.
