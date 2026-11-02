import React from 'react'
import { Shell } from './components/Shell'
import { Toasts } from './components/Toasts'
import { usePage } from './lib/router'
import { OverviewPage } from './pages/Overview'
import { DrivePage } from './pages/Drive'
import { ImuPage } from './pages/Imu'
import { SensorsPage } from './pages/Sensors'
import { ServoPage } from './pages/Servo'
import { ParamsPage } from './pages/Params'
import { TasksPage } from './pages/Tasks'
import { ScopePage } from './pages/Scope'
import { ControlPage } from './pages/Control'
import { SettingsPage } from './pages/Settings'

export default function App() {
  const page = usePage()
  return (
    <>
      <Shell>
        {page === 'overview' && <OverviewPage />}
        {page === 'drive' && <DrivePage />}
        {page === 'imu' && <ImuPage />}
        {page === 'sensors' && <SensorsPage />}
        {page === 'servo' && <ServoPage />}
        {page === 'params' && <ParamsPage />}
        {page === 'tasks' && <TasksPage />}
        {page === 'scope' && <ScopePage />}
        {page === 'control' && <ControlPage />}
        {page === 'settings' && <SettingsPage />}
      </Shell>
      <Toasts />
    </>
  )
}
