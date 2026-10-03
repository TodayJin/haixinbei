/**
 * 无浏览器冒烟测试：用 react-dom/server 把 9 个页面各渲染一遍（空状态、没有 SSE、没有 fetch）。
 *
 * 为什么用 SSR 而不是 jsdom：本任务只允许 react/react-dom/uplot/lucide-react 这几个运行时依赖，
 * 不想为了截图再拉 puppeteer/jsdom。SSR 能把"组件体 + JSX"整条路径跑掉，足以抓住：
 * 渲染期异常、对 null 遥测的裸取字段、JSX 结构错误、import 循环。
 * （真"像素级"检查见 README 里的无头 Chrome 命令。）
 */
import { describe, expect, it } from 'vitest'
import { renderToStaticMarkup } from 'react-dom/server'
import { OverviewPage } from '../pages/Overview'
import { DrivePage } from '../pages/Drive'
import { ImuPage } from '../pages/Imu'
import { SensorsPage } from '../pages/Sensors'
import { ServoPage } from '../pages/Servo'
import { ParamsPage } from '../pages/Params'
import { ScopePage } from '../pages/Scope'
import { ControlPage } from '../pages/Control'
import { SettingsPage } from '../pages/Settings'

const CASES: Array<[string, () => React.JSX.Element, string]> = [
  ['总览', OverviewPage, '循迹'],
  ['操控', DrivePage, 'WASD'],
  ['IMU', ImuPage, '偏航'],
  ['传感器', SensorsPage, '灰度'],
  ['舵机与动作', ServoPage, '舵机'],
  ['调参', ParamsPage, '参数'],
  ['波形', ScopePage, 'CSV'],
  ['控制与日志', ControlPage, '任务'],
  ['设置', SettingsPage, '串口'],
]

describe('9 个页面 SSR 冒烟', () => {
  for (const [name, Page, marker] of CASES) {
    it(`${name} 能渲染且含关键文案「${marker}」`, () => {
      const html = renderToStaticMarkup(<Page />)
      expect(html.length).toBeGreaterThan(400)
      expect(html).toContain(marker)
      // 空状态下不能出现 NaN / undefined 直接漏到界面
      expect(html).not.toContain('NaN')
      expect(html).not.toContain('>undefined<')
    })
  }
})
