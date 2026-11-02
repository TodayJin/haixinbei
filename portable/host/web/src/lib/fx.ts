/**
 * 两个「滚起来别掉帧」的开关：都是给 `<html>` 挂一个类，样式写在 index.css 里。
 *
 * 为什么需要（2026-10-04 真车跑着 100Hz 遥测、用 CDP tracing 量出来的）：
 *  - 玻璃模糊（`backdrop-filter`）在**滚动过程中**每帧都要重新模糊一遍：
 *    tasks 页合成器 `DirectRenderer::DrawFrame` 332ms → 55.7ms / 2.5s（省 6 倍），
 *    其中 `.glass` 卡片占一半、sticky header 占四分之一、侧栏几乎不占。
 *  - 无限脉冲动画（`animate-pulseDot`，连接正常时 header 那个圆点一直亮着）在**不做
 *    合成器动画**的机器上，每帧一次全文档样式重算（`RecalcStyleCount` = 帧数）。实测
 *    `animation:none` 后 styleN 从 650 掉到 50；换成 `steps()` 或只动 opacity 都不管用。
 *  - 灰度柱/趋势条的 `transition-[height]`（**布局属性**）被 20Hz 遥测反复重启 ⇒ 过渡
 *    永远在跑 ⇒ 总览/传感器页每帧一次全文档重排（layoutN 650 → 51）、光栅化 8 倍。
 *    （那一条已在 `components/ui.tsx` 里改成 `transition-colors`，不靠这里的开关。）
 *
 * 两条都"好看但贵"，所以：**滚动期间自动关掉**（松手 SCROLL_IDLE_MS 之后恢复），
 * 机器实在弱（远程桌面/虚拟显示）还可以在设置页把「低特效模式」常开。
 */

/** 正在滚动时挂在 `<html>` 上的类 */
export const SCROLLING_CLASS = 'is-scrolling'
/** 「低特效模式」常开时挂在 `<html>` 上的类 */
export const LITE_CLASS = 'perf-lite'

/**
 * 松手后多久算"不滚了"（毫秒）。
 * 太短：惯性滚动里反复开关，模糊一闪一闪；太长：静止画面多糊一会儿。
 */
export const SCROLL_IDLE_MS = 140

export interface ClassListLike {
  add(name: string): void
  remove(name: string): void
}

/** 低特效模式：挂/摘 `<html classList>` 上的类 */
export function applyLiteFx(target: ClassListLike, on: boolean): void {
  if (on) target.add(LITE_CLASS)
  else target.remove(LITE_CLASS)
}

export interface ScrollFxTimers {
  setTimer: (fn: () => void, ms: number) => unknown
  clearTimer: (handle: unknown) => void
}

/**
 * 滚动状态机：第一次 `tick()` 挂类，之后每次 `tick()` 把定时器往后推，
 * 静默 `SCROLL_IDLE_MS` 毫秒后摘类。时间源注入 ⇒ 纯逻辑，node 环境里能单测。
 */
export function createScrollFx(target: ClassListLike, timers: ScrollFxTimers) {
  let handle: unknown = null
  let scrolling = false

  const clear = (): void => {
    if (handle !== null) {
      timers.clearTimer(handle)
      handle = null
    }
  }

  return {
    get scrolling(): boolean {
      return scrolling
    },
    /** 收到一次 scroll 事件 */
    tick(): void {
      if (!scrolling) {
        scrolling = true
        target.add(SCROLLING_CLASS)
      }
      clear()
      handle = timers.setTimer(() => {
        handle = null
        scrolling = false
        target.remove(SCROLLING_CLASS)
      }, SCROLL_IDLE_MS)
    },
    /** 卸载：清定时器、摘类（组件卸载后类不能留在 `<html>` 上） */
    dispose(): void {
      clear()
      if (scrolling) {
        scrolling = false
        target.remove(SCROLLING_CLASS)
      }
    },
  }
}

/** 把上面的状态机接到 `window` 的 scroll 上（浏览器里用），返回卸载函数 */
export function installScrollFx(win: Window, doc: Document): () => void {
  const fx = createScrollFx(doc.documentElement.classList, {
    setTimer: (fn, ms) => win.setTimeout(fn, ms),
    clearTimer: (h) => win.clearTimeout(h as number),
  })
  const onScroll = (): void => fx.tick()
  win.addEventListener('scroll', onScroll, { passive: true })
  return () => {
    win.removeEventListener('scroll', onScroll)
    fx.dispose()
  }
}
