import type { CadProcessOptions, CadSystem } from "./process.js";
export declare function windowsCadHelper(): Promise<string>;
export declare function windowsCadRuntime(data: string): Promise<{
    base: string;
    python: string;
    site: string;
    assets: string;
}>;
export declare function prepareWindowsCad(system: CadSystem, data: string, source: string, opts: CadProcessOptions): Promise<void>;
/** Stage per-call copies so the OS never grants an executed design access to its saved checkpoints. */
export declare function runWindowsCad(system: CadSystem, data: string, mode: string, sourceRequest: string, job: string, protectedFiles: string[], opts: CadProcessOptions): Promise<{
    stdout: string;
    stderr?: string;
}>;
