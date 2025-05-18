         ┌────────────┐
         │sensor_task │
         └─────┬──────┘
               │
      updates shared sensor_data
               │
     ┌─────────▼─────────┐        ┌────────────────────┐
     │     lcd_task      │        │ notification_task   │
     │ (display MEMLCD)  │        │ (send if connected) │
     └─────────┬─────────┘        └──────────┬──────────┘
               │                             │
               │                             |
               │                             ▲
               │                    ┌────────┴────────┐
               └───────────────────►│    bond_task    │
                                    │ (reconnect only)│
                                    └─────────────────┘
