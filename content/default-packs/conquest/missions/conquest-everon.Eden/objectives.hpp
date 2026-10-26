// Everon's objectives for Conquest (included by conquest.hpp inside class CfgConquest).
// Positions are CfgWorlds >> Eden >> Names (map metres, x east, y north); the airfield is its ilsPosition.
// Every village of Everon. HQs fall last; a side may only attack an objective next to one it holds.
class Objectives
{
    class Morton       { name = "Morton";       position[] = {5134, 3955};  radius = 120; value = 10; kind = "HQ"; owner = "WEST"; neighbours[] = {"Figari", "Regina"}; };
    class Regina       { name = "Regina";       position[] = {7197, 2347};  radius = 120; value = 5;  neighbours[] = {"Durras", "Levie"}; };
    class Durras       { name = "Durras";       position[] = {8835, 2710};  radius = 120; value = 5;  neighbours[] = {"Vernon", "Levie"}; };
    class Vernon       { name = "Vernon";       position[] = {9142, 2159};  radius = 100; value = 5;  neighbours[] = {"SaintPierre"}; };
    class SaintPierre  { name = "Saint Pierre"; position[] = {9662, 1605};  radius = 100; value = 5;  };
    class Levie        { name = "Levie";        position[] = {7554, 4737};  radius = 100; value = 5;  neighbours[] = {"Laruns", "Figari"}; };
    class Laruns       { name = "Laruns";       position[] = {7556, 5558};  radius = 100; value = 5;  neighbours[] = {"Chotain"}; };
    class Chotain      { name = "Chotain";      position[] = {7057, 5987};  radius = 100; value = 5;  neighbours[] = {"Provins", "EntreDeux"}; };
    class Figari       { name = "Figari";       position[] = {5283, 5375};  radius = 120; value = 5;  neighbours[] = {"Provins", "Montignac", "LeMoule"}; };
    class Provins      { name = "Provins";      position[] = {5501, 6050};  radius = 100; value = 5;  neighbours[] = {"Montignac", "EntreDeux"}; };
    class EntreDeux    { name = "Entre Deux";   position[] = {5735, 7059};  radius = 120; value = 5;  neighbours[] = {"Montignac", "Tyrone"}; };
    class Montignac    { name = "Montignac";    position[] = {4935, 6994};  radius = 150; value = 10; neighbours[] = {"Gravette", "Tyrone"}; };
    class LeMoule      { name = "Le Moule";     position[] = {2590, 5384};  radius = 120; value = 10; kind = "HQ"; owner = "GUER"; neighbours[] = {"Lamentin", "Gravette"}; };
    class Lamentin     { name = "Lamentin";     position[] = {1333, 5962};  radius = 120; value = 5;  };
    class Gravette     { name = "Gravette";     position[] = {4112, 7772};  radius = 120; value = 5;  neighbours[] = {"Tyrone"}; };
    class Tyrone       { name = "Tyrone";       position[] = {4946, 9056};  radius = 120; value = 5;  neighbours[] = {"Meaux", "Airfield"}; };
    class Meaux        { name = "Meaux";        position[] = {4524, 9549};  radius = 120; value = 5;  neighbours[] = {"Airfield"}; };
    class Airfield     { name = "Airfield";     position[] = {4772, 10923}; radius = 200; value = 10; kind = "HQ"; owner = "EAST"; };
};
